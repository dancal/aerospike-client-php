#include "php.h"
#include "ext/standard/php_var.h"
#include "ext/standard/php_smart_str.h"

#include "aerospike/as_status.h"
#include "aerospike/as_config.h"
#include "aerospike/aerospike_key.h"
#include "aerospike/as_hashmap.h"
#include "aerospike/as_arraylist.h"
#include "aerospike/as_bytes.h"

#include "aerospike_common.h"
#include "aerospike_transform.h"
#include "aerospike_policy.h"

#include "aerospike/as_record.h"
#include "aerospike/as_record_iterator.h"
#include <ext/standard/base64.h>
/*
 *******************************************************************************************************
 * MACRO TO COMPARE TWO KEYS OF A PHP ARRAY
 *******************************************************************************************************
 */
#define PHP_COMPARE_KEY(key_const, key_const_len, key_obtained, key_obtained_len)    \
     ((key_const_len == key_obtained_len) && (0 == memcmp(key_obtained, key_const, key_const_len)))

/*
 *******************************************************************************************************
 * Forward declarations of certain helper methods for PUT/GET.
 *******************************************************************************************************
 */
void AS_DEFAULT_PUT(void *key, void *value, as_record *record,
                    void *static_pool, uint32_t serializer_policy,
                    as_error *error_p TSRMLS_DC);
void AS_LIST_PUT(void *key, void *value, void *store,
                 void *static_pool, uint32_t serializer_policy,
                 as_error *error_p TSRMLS_DC);
void AS_MAP_PUT(void *key, void *value, void *store,
                void *static_pool, uint32_t serializer_policy,
                as_error *error_p TSRMLS_DC);
bool AS_LIST_GET_CALLBACK(as_val *value, void *array);
bool AS_MAP_GET_CALLBACK(as_val *key, as_val *value, void *array);
static as_status
aerospike_transform_iteratefor_addr_port(HashTable* ht_p,
        void* as_config_p);

/* 
 *******************************************************************************************************
 * PHP Userland Serializer callback.
 *******************************************************************************************************
 */
zend_fcall_info       user_serializer_call_info;
zend_fcall_info_cache user_serializer_call_info_cache;
zval                  *user_serializer_callback_retval_p;
uint32_t              is_user_serializer_registered = 0;

/* 
 *******************************************************************************************************
 * PHP Userland Deserializer callback.
 *******************************************************************************************************
 */
zend_fcall_info       user_deserializer_call_info;
zend_fcall_info_cache user_deserializer_call_info_cache;
zval                  *user_deserializer_callback_retval_p;
uint32_t              is_user_deserializer_registered = 0;

/*
 *******************************************************************************************************
 * Sets value of as_bytes with bytes from bytes_string.
 * Sets type of as_bytes to bytes_type.
 *
 * @param bytes                 The C client's as_bytes to be set.
 * @param bytes_string          The bytes string to be set into as_bytes.
 * @param bytes_string_len      The length of bytes string.
 * @param bytes_type            The type of as_bytes to be set.
 * @param error_p               The as_error to be populated by the function
 *                              with encountered error if any.
 *******************************************************************************************************
 */
static void set_as_bytes(as_bytes *bytes,
                         uint8_t *bytes_string,
                         int32_t bytes_string_len,
                         int32_t bytes_type,
                         as_error *error_p TSRMLS_DC)
{
    if((!bytes) || (!bytes_string)) {
        DEBUG_PHP_EXT_ERROR("Unable to set as_bytes");
        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR, "Unable to set as_bytes");
        goto exit;
    }

    as_bytes_init(bytes, bytes_string_len);

    if (!as_bytes_set(bytes, 0, bytes_string, bytes_string_len)) {
        DEBUG_PHP_EXT_ERROR("Unable to set as_bytes");
        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR, "Unable to set as_bytes");
    } else {
        as_bytes_set_type(bytes, bytes_type);
        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_OK, DEFAULT_ERROR);
    }
exit:
    return;
}

/*
 *******************************************************************************************************
 * If serialize_flag == true, executes the passed userland serializer callback,
 * by creating as_bytes (bytes) from the passed zval (value).
 * Else executes the passed userland deserializer callback,
 * by passing the as_bytes (bytes) to the deserializer and getting back
 * the corresponding zval (value).
 *
 * @param user_callback_info            The zend_fcall_info for the user
 *                                      callback to be executed.
 * @param user_callback_info_cache      The zend_fcall_info_cache for the user
 *                                      callback to be executed.
 * @param user_callback_retval_p        The return value for the user callback
 *                                      to be executed.
 * @param bytes                         The as_bytes to be stored/retrieved.
 * @param value                         The value to be retrieved/stored.
 * @param serialize_flag                The flag which indicates
 *                                      serialize/deserialize.
 * @param error_p                       The as_error to be populated by the
 *                                      function with encountered error if any.
 *******************************************************************************************************
 */
static void execute_user_callback(zend_fcall_info *user_callback_info,
                                  zend_fcall_info_cache *user_callback_info_cache,
                                  zval *user_callback_retval_p,
                                  as_bytes *bytes,
                                  zval **value,
                                  bool serialize_flag,
                                  as_error *error_p TSRMLS_DC)
{
    zval**      params[1];
    zval*       bytes_string = NULL;
    char*       bytes_val_p = (char*)bytes->value;

    ALLOC_INIT_ZVAL(bytes_string);

    if (serialize_flag) {
        params[0] = value;
    } else {
        ZVAL_STRINGL(bytes_string, bytes_val_p, bytes->size, 1);
        params[0] = &bytes_string;
    }

    user_callback_info->param_count = 1;
    user_callback_info->params = params;
    user_callback_info->retval_ptr_ptr = &user_callback_retval_p;

    if (zend_call_function(user_callback_info,
                user_callback_info_cache TSRMLS_CC) == SUCCESS &&
                        user_callback_info->retval_ptr_ptr && 
                            *user_callback_info->retval_ptr_ptr) {

        if (serialize_flag) {
            COPY_PZVAL_TO_ZVAL(*bytes_string,
                    *user_callback_info->retval_ptr_ptr);
            set_as_bytes(bytes, (uint8_t*)Z_STRVAL_P(bytes_string),
                         bytes_string->value.str.len, AS_BYTES_BLOB, error_p TSRMLS_CC);
        } else {
            COPY_PZVAL_TO_ZVAL(**value, *user_callback_info->retval_ptr_ptr);
            PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_OK, DEFAULT_ERROR);
        }
    } else {
        if (serialize_flag) {
            DEBUG_PHP_EXT_ERROR("Unable to call user's registered serializer callback");
            PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                    "Unable to call user's registered serializer callback");
        } else {
            DEBUG_PHP_EXT_ERROR("Unable to call user's registered deserializer callback");
            PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                    "Unable to call user's registered deserializer callback");
        }
    }

    zval_ptr_dtor(&bytes_string);
}

/*
 *******************************************************************************************************
 * Checks serializer_policy.
 * Serializes zval (value) into as_bytes using serialization logic
 * based on serializer_policy.
 *
 * @param serializer_policy         The serializer_policy to be used to handle
 *                                  the serialization.
 * @param bytes                     The as_bytes to be set.
 * @param value                     The value to be serialized.
 * @param error_p                   The as_error to be populated by the function
 *                                  with encountered error if any.
 *******************************************************************************************************
 */
static void serialize_based_on_serializer_policy(int32_t serializer_policy,
                                                 as_bytes *bytes,
                                                 zval **value,
                                                 as_error *error_p TSRMLS_DC)
{
    switch(serializer_policy) {
        case SERIALIZER_NONE:
            PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
                    "Cannot serialize: SERIALIZER_NONE selected");
            goto exit;
        case SERIALIZER_PHP:
            {
                php_serialize_data_t var_hash;
                smart_str buf = {0};
                PHP_VAR_SERIALIZE_INIT(var_hash);
                php_var_serialize(&buf, value, &var_hash TSRMLS_CC);
                PHP_VAR_SERIALIZE_DESTROY(var_hash);
                if (EG(exception)) {
                    smart_str_free(&buf);
                    DEBUG_PHP_EXT_ERROR("Unable to serialize using standard php serializer");
                    PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                            "Unable to serialize using standard php serializer");
                    goto exit;
                } else if (buf.c) {
                    set_as_bytes(bytes, (uint8_t*)buf.c, buf.len, AS_BYTES_PHP, error_p TSRMLS_CC);
                    if (AEROSPIKE_OK != (error_p->code)) {
                        smart_str_free(&buf);
                        goto exit;
                    }
                } else {
                    smart_str_free(&buf);
                    DEBUG_PHP_EXT_ERROR("Unable to serialize using standard php serializer");
                    PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                            "Unable to serialize using standard php serializer");
                    goto exit;
                }
                smart_str_free(&buf);
            }
            break;
        case SERIALIZER_JSON:
                /*
                 *   TODO:
                 *     Handle JSON serialization after support for AS_BYTES_JSON
                 *     is added in aerospike-client-c
                 */
                 DEBUG_PHP_EXT_ERROR("Unable to serialize using standard json serializer");
                 PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                         "Unable to serialize using standard json serializer");
                 goto exit;
        case SERIALIZER_USER:
            DEBUG_PHP_EXT_DEBUG("Should come here");
            if (is_user_serializer_registered) {
                execute_user_callback(&user_serializer_call_info,
                                      &user_serializer_call_info_cache,
                                      user_serializer_callback_retval_p,
                                      bytes, value, true, error_p TSRMLS_CC);
                if (AEROSPIKE_OK != (error_p->code)) {
                    goto exit;
                }
            } else {
                DEBUG_PHP_EXT_ERROR("No serializer callback registered");
                PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                        "No serializer callback registered");
                goto exit;
            }
            break;
        default:
            DEBUG_PHP_EXT_ERROR("Unsupported serializer");
            PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                    "Unsupported serializer");
            goto exit;
    }
    PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_OK, DEFAULT_ERROR);
exit:
    return;
}

/*
 *******************************************************************************************************
 * Checks as_bytes->type.
 * Unserializes as_bytes into zval (retval) using unserialization logic
 * based on as_bytes->type.
 *
 * @param bytes                 The as_bytes to be deserialized.
 * @param retval                The return zval to be populated with the
 *                              deserialized value of the input as_bytes.
 * @param error_p               The as_error to be populated by the function
 *                              with encountered error if any.
 *******************************************************************************************************
 */
static void unserialize_based_on_as_bytes_type(as_bytes  *bytes,
                                               zval      **retval,
                                               as_error  *error_p TSRMLS_DC)
{
    int8_t*     bytes_val_p = NULL;

    if (!bytes || !(bytes->value)) {
        DEBUG_PHP_EXT_DEBUG("Invalid bytes");
        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR, "Invalid bytes");
        goto exit;
    }

    bytes_val_p = (int8_t*) bytes->value;

    ALLOC_INIT_ZVAL(*retval);

    switch(as_bytes_get_type(bytes)) {
        case AS_BYTES_PHP: {
                php_unserialize_data_t var_hash;
                PHP_VAR_UNSERIALIZE_INIT(var_hash);
                if (1 != php_var_unserialize(retval,
                            (const unsigned char **) &(bytes_val_p),
                            (const unsigned char*) ((char *) bytes_val_p + bytes->size),
                            &var_hash TSRMLS_CC)) {
                    DEBUG_PHP_EXT_ERROR("Unable to unserialize bytes using standard php unserializer");
                    PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                            "Unable to unserialize bytes using standard php unserializer");
                    PHP_VAR_UNSERIALIZE_DESTROY(var_hash);
                    goto exit;
                }
                PHP_VAR_UNSERIALIZE_DESTROY(var_hash);
            }
            break;
        case AS_BYTES_BLOB: {
                if (is_user_deserializer_registered) {
                    execute_user_callback(&user_deserializer_call_info,
                                          &user_deserializer_call_info_cache,
                                          user_deserializer_callback_retval_p,
                                          bytes, retval, false, error_p TSRMLS_CC);
                    if(AEROSPIKE_OK != (error_p->code)) {
                        goto exit;
                    }
                } else {
                    DEBUG_PHP_EXT_ERROR("No unserializer callback registered");
                    PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                            "No unserializer callback registered");
                    goto exit;
                }
            }
            break;
        default:
            DEBUG_PHP_EXT_ERROR("Unable to unserialize bytes");
            PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                    "Unable to unserialize bytes");
            goto exit;
    }

    PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_OK, DEFAULT_ERROR);
exit:
    return;
}

/* 
 *******************************************************************************************************
 * GET helper functions.
 *******************************************************************************************************
 */

/* 
 *******************************************************************************************************
 * Wrappers for appeding datatype to List.
 *******************************************************************************************************
 */

/*
 *******************************************************************************************************
 * Appends a NULL to PHP indexed array: list.
 *
 * @param key                   The key for the array (NULL).
 * @param value                 The NULL value to be appended to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_LIST_APPEND_NULL(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    add_next_index_null(*((zval **) array));
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Appends a boolean to PHP indexed array: list.
 *
 * @param key                   The key for the array (NULL).
 * @param value                 The boolean value to be appended to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_LIST_APPEND_BOOL(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    add_next_index_bool(*((zval **) array),
            (int8_t) as_boolean_get((as_boolean *) value));
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Appends a long to PHP indexed array: list.
 *
 * @param key                   The key for the array (NULL).
 * @param value                 The boolean value to be appended to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_LIST_APPEND_LONG(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    add_next_index_long(*((zval **) array),
            (long) as_integer_get((as_integer *) value));
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Appends a string to PHP indexed array: list.
 *
 * @param key                   The key for the array (NULL).
 * @param value                 The string value to be appended to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_LIST_APPEND_STRING(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    add_next_index_stringl(*((zval **) array),
            as_string_get((as_string *) value),
            strlen(as_string_get((as_string *) value)), 1);
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Appends a rec to PHP indexed array: list.
 *
 * @param key                   The key for the array (NULL).
 * @param value                 The record value to be appended to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_LIST_APPEND_REC(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Appends a pair to PHP indexed array: list.
 *
 * @param key                   The key for the array (NULL).
 * @param value                 The pair value to be appended to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_LIST_APPEND_PAIR(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Appends bytes to PHP indexed array: list.
 *
 * @param key                   The key for the array (NULL).
 * @param value                 The bytes value to be appended to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_LIST_APPEND_BYTES(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    zval        *unserialized_zval = NULL;

    unserialize_based_on_as_bytes_type((as_bytes *) value,
                                &unserialized_zval, (as_error *) err TSRMLS_CC);

    if (AEROSPIKE_OK != ((as_error *) err)->code) {
        DEBUG_PHP_EXT_ERROR("Unable to unserialize bytes");
        goto exit;
    }
    add_next_index_zval(*((zval **) array), unserialized_zval);
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);

exit:
    if (AEROSPIKE_OK != ((as_error *) err)->code) {
        if (unserialized_zval)
            zval_ptr_dtor(&unserialized_zval);
    }
    return;
}

/* 
 *******************************************************************************************************
 * Wrappers for associating datatype with Map with string key.
 *******************************************************************************************************
 */

/*
 *******************************************************************************************************
 * Adds a NULL to PHP assoc array: map with string key.
 *
 * @param key                   The key for the assoc array.
 * @param value                 The NULL value to be added to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_MAP_ASSOC_NULL(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    add_assoc_null(*((zval **) array), as_string_get((as_string *) key));
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Adds a boolean to PHP assoc array: map with string key.
 *
 * @param key                   The key for the assoc array.
 * @param value                 The boolean value to be added to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_MAP_ASSOC_BOOL(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    add_assoc_bool(*((zval **) array), as_string_get((as_string *) key),
            (int) as_boolean_get((as_boolean *) value));
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Adds a long to PHP assoc array: map with string key.
 *
 * @param key                   The key for the assoc array.
 * @param value                 The long value to be added to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_MAP_ASSOC_LONG(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    add_assoc_long(*((zval **) array),  as_string_get((as_string *) key),
            (long) as_integer_get((as_integer *) value));
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Adds a string to PHP assoc array: map with string key.
 *
 * @param key                   The key for the assoc array.
 * @param value                 The string value to be added to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_MAP_ASSOC_STRING(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    add_assoc_stringl(*((zval **) array), as_string_get((as_string *) key),
            as_string_get((as_string *) value),
            strlen(as_string_get((as_string *) value)), 1);
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Adds a rec to PHP assoc array: map with string key.
 *
 * @param key                   The key for the assoc array.
 * @param value                 The rec value to be added to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_MAP_ASSOC_REC(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Adds a pair to PHP assoc array: map with string key.
 *
 * @param key                   The key for the assoc array.
 * @param value                 The pair value to be added to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_MAP_ASSOC_PAIR(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Adds bytes to PHP assoc array: map with string key.
 *
 * @param key                   The key for the assoc array.
 * @param value                 The bytes value to be added to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_MAP_ASSOC_BYTES(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    zval        *unserialized_zval = NULL;

    unserialize_based_on_as_bytes_type((as_bytes *) value,
                                &unserialized_zval, (as_error *) err TSRMLS_CC);
    if (AEROSPIKE_OK != ((as_error *) err)->code) {
        DEBUG_PHP_EXT_ERROR("Unable to unserialize bytes");
        goto exit;
    }
    add_assoc_zval(*((zval **) array), as_string_get((as_string *) key),
            unserialized_zval);
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);

exit:
    return;
}

/*
 *******************************************************************************************************
 * Wrappers for associating datatype with Map with integer key.
 *******************************************************************************************************
 */
 
/*
 *******************************************************************************************************
 * Adds a null to PHP assoc array at specified index: map with integer key.
 * 
 * @param key                   The index at which value is to be added in the assoc array.
 * @param value                 The null value to be added to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_MAP_INDEX_NULL(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    add_index_null(*((zval **) array), (uint) as_integer_get((as_integer *) key));
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Adds a boolean to PHP assoc array at specified index: map with integer key.
 *
 * @param key                   The index at which value is to be added in the assoc array.
 * @param value                 The boolean value to be added to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_MAP_INDEX_BOOL(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    add_index_bool(*((zval **) array), (uint) as_integer_get((as_integer *) key),
            (int) as_boolean_get((as_boolean *) value));
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Adds a long to PHP assoc array at specified index: map with integer key.
 *
 * @param key                   The index at which value is to be added in the assoc array.
 * @param value                 The long value to be added to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_MAP_INDEX_LONG(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    add_index_long(*((zval **) array), (uint) as_integer_get((as_integer *) key),
            (long) as_integer_get((as_integer *) value));
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Adds a string to PHP assoc array at specified index: map with integer key.
 *
 * @param key                   The index at which value is to be added in the assoc array.
 * @param value                 The string value to be added to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_MAP_INDEX_STRING(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    add_index_stringl(*((zval**)array), (uint) as_integer_get((as_integer *) key),
            as_string_get((as_string *) value),
            strlen(as_string_get((as_string *) value)), 1);
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Adds a rec to PHP assoc array at specified index: map with integer key.
 *
 * @param key                   The index at which value is to be added in the assoc array.
 * @param value                 The rec value to be added to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_MAP_INDEX_REC(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Adds a pair to PHP assoc array at specified index: map with integer key.
 *
 * @param key                   The index at which value is to be added in the assoc array.
 * @param value                 The pair value to be added to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_MAP_INDEX_PAIR(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Adds bytes to PHP assoc array at specified index: map with integer key.
 *
 * @param key                   The index at which value is to be added in the assoc array.
 * @param value                 The bytes value to be added to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_MAP_INDEX_BYTES(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    zval        *unserialized_zval = NULL;

    unserialize_based_on_as_bytes_type((as_bytes *) value,
                                &unserialized_zval, (as_error *) err TSRMLS_CC);
    if (AEROSPIKE_OK != ((as_error *) err)->code) {
        DEBUG_PHP_EXT_ERROR("Unable to unserialize bytes");
        goto exit;
    }
    add_index_zval(*((zval**)array), (uint) as_integer_get((as_integer *) key),
            unserialized_zval);
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);

exit:
    if (AEROSPIKE_OK != ((as_error *) err)->code) {
        if (unserialized_zval)
            zval_ptr_dtor(&unserialized_zval);
    }
    return;
}

/*
 *******************************************************************************************************
 * Wrappers for associating datatype with Record.
 *******************************************************************************************************
 */

/*
 *******************************************************************************************************
 * Adds a NULL to PHP assoc array: record.
 *
 * @param key                   The bin name.
 * @param value                 The null value to be added to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_DEFAULT_ASSOC_NULL(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    /*
     * key will be NULL in case of UDF methods.
     * NULL will differentiate UDF from normal GET calls.
     */
    if (key == NULL) {
        //ZVAL_NULL((zval *) array);
    } else {
        add_assoc_null(((zval *) array), (char *) key);
    }
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Adds a boolean to PHP assoc array: record.
 *
 * @param key                   The bin name.
 * @param value                 The boolean value to be added to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_DEFAULT_ASSOC_BOOL(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    /*
     * key will be NULL in case of UDF methods.
     * NULL will differentiate UDF from normal GET calls.
     */
    if (key == NULL) {
        zval* bool_zval_p = NULL;
        ALLOC_INIT_ZVAL(bool_zval_p);
        ZVAL_BOOL(bool_zval_p, (int) as_boolean_get((as_boolean *) value));
        zval_dtor((zval *)array);
        ZVAL_ZVAL((zval *)array, bool_zval_p, 1, 1);
    } else {
        add_assoc_bool(((zval *) array), (char *) key,
                (int) as_boolean_get((as_boolean *) value));
    }
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Adds a long to PHP assoc array: record.
 *
 * @param key                   The bin name.
 * @param value                 The long value to be added to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_DEFAULT_ASSOC_LONG(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    /*
     * key will be NULL in case of UDF methods.
     * NULL will differentiate UDF from normal GET calls.
     */
    if (key == NULL) {
        zval* long_zval_p = NULL;
        ALLOC_INIT_ZVAL(long_zval_p);
        ZVAL_LONG(long_zval_p, (long) as_integer_get((as_integer *) value));
        zval_dtor((zval *)array);
        ZVAL_ZVAL((zval *)array, long_zval_p, 1, 1);
    } else {
       add_assoc_long(((zval *) array),  (char *) key,
               (long) as_integer_get((as_integer *) value));
    }
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Adds a string to PHP assoc array: record.
 *
 * @param key                   The bin name.
 * @param value                 The string value to be added to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_DEFAULT_ASSOC_STRING(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    /*
     * key will be NULL in case of UDF methods.
     * NULL will differentiate UDF from normal GET calls.
     */
    if (key == NULL) {
        zval* string_zval_p = NULL;
        ALLOC_INIT_ZVAL(string_zval_p);
        //ZVAL_STRINGL(string_zval_p, as_string_get((as_string *) value), as_string_len(as_string_get((as_string *) value)), 1);
		ZVAL_STRINGL(string_zval_p, as_string_get((as_string *) value), as_string_len((as_string *) value), 1);

        zval_dtor((zval *)array);
        ZVAL_ZVAL((zval *)array, string_zval_p, 1, 1);
    } else {
        //add_assoc_stringl(((zval *) array), (char *) key, as_string_get((as_string *) value), as_string_len(as_string_get((as_string *) value)), 1);
		add_assoc_stringl(((zval *) array), (char *) key, as_string_get((as_string *) value), as_string_len((as_string *) value), 1);
    }

    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Adds a rec to PHP assoc array: record.
 *
 * @param key                   The bin name.
 * @param value                 The rec value to be added to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_DEFAULT_ASSOC_REC(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Adds a pair to PHP assoc array: record.
 *
 * @param key                   The bin name.
 * @param value                 The pair value to be added to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_DEFAULT_ASSOC_PAIR(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Adds a NULL to PHP assoc array: record.
 *
 * @param key                   The bin name.
 * @param value                 The null value to be added to the PHP array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_DEFAULT_ASSOC_BYTES(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    zval        *unserialized_zval = NULL;

    unserialize_based_on_as_bytes_type((as_bytes *) value,
                                &unserialized_zval, (as_error *) err TSRMLS_CC);
    if (AEROSPIKE_OK != ((as_error *) err)->code) {
        DEBUG_PHP_EXT_ERROR("Unable to unserialize bytes");
        goto exit;
    }

    /*
     * key will be NULL in case of UDF methods.
     * NULL will differentiate UDF from normal GET calls.
     */
    if (key == NULL) {
        ZVAL_ZVAL((zval *) array, unserialized_zval, 1, 1);
    } else {
        add_assoc_zval(((zval*)array), (char*) key, unserialized_zval);
    }
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);

exit:
    if (AEROSPIKE_OK != ((as_error *) err)->code) {
        if (unserialized_zval)
            zval_ptr_dtor(&unserialized_zval);
    }
    return;
}

/* 
 *******************************************************************************************************
 * GET helper functions with expanding macros.
 *******************************************************************************************************
 */

/*
 *******************************************************************************************************
 * Appends a map to PHP indexed array: list.
 *
 * @param key                   The bin name.
 * @param value                 The PHP assoc array: map value to be added to the PHP indexed array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_LIST_APPEND_MAP(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    AS_APPEND_MAP_TO_LIST(key, value, array, err);
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Appends a list to PHP indexed array: list.
 *
 * @param key                   The bin name.
 * @param value                 The PHP indexed array: list value to be added to the PHP indexed array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_LIST_APPEND_LIST(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    AS_APPEND_LIST_TO_LIST(key, value, array, err);
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Appends a map to PHP assoc array: map.
 *
 * @param key                   The bin name.
 * @param value                 The PHP assoc array: map value to be added to the PHP assoc array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_MAP_ASSOC_MAP(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    AS_ASSOC_MAP_TO_MAP(key, value, array, err);
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Appends a list to PHP assoc array: map.
 *
 * @param key                   The bin name.
 * @param value                 The PHP indexed array: list value to be added to the PHP assoc array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_MAP_ASSOC_LIST(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    AS_ASSOC_LIST_TO_MAP(key, value, array, err);
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Appends a map to PHP assoc array at specific index: map with integer key.
 *
 * @param key                   The bin name.
 * @param value                 The PHP assoc array: map value to be added to the PHP assoc array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_MAP_INDEX_MAP(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    AS_INDEX_MAP_TO_MAP(key, value, array, err);
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Appends a list to PHP assoc array at specific index: map with integer key.
 *
 * @param key                   The bin name.
 * @param value                 The PHP indexed array: list value to be added to the PHP assoc array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_MAP_INDEX_LIST(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    AS_INDEX_LIST_TO_MAP(key, value, array, err);
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Appends a map to PHP assoc array: record
 *
 * @param key                   The bin name.
 * @param value                 The PHP assoc array: map value to be added to the PHP assoc array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_DEFAULT_ASSOC_MAP(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    if ((NULL == key)) {
        zval_dtor((zval *)array);
    }
    AS_ASSOC_MAP_TO_DEFAULT(key, value, array, err);
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}

/*
 *******************************************************************************************************
 * Appends a list to PHP assoc array: record
 *
 * @param key                   The bin name.
 * @param value                 The PHP indexed array: list value to be added to the PHP assoc array.
 * @param array                 The PHP array to be appended to.
 * @param err                   The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void ADD_DEFAULT_ASSOC_LIST(void *key, void *value, void *array, void *err TSRMLS_DC)
{
    if (NULL == key) {
        zval_dtor((zval *)array);
    }
    AS_ASSOC_LIST_TO_DEFAULT(key, value, array, err);
    PHP_EXT_SET_AS_ERR((as_error *) err, AEROSPIKE_OK, DEFAULT_ERROR);
}


/*
 *******************************************************************************************************
 * GET callback methods where switch case will expand.
 *******************************************************************************************************
 */

/*
 *******************************************************************************************************
 * Callback function for as_record_foreach.
 *
 * @param key                   The bin name.
 * @param value                 The current bin value.
 * @param array                 The foreach_callback_udata struct containing the PHP record array 
 *                              as well as the as_error to be populated by the callback.
 *
 * @return true if the callback succeeds. Otherwise false.
 *******************************************************************************************************
 */
extern bool AS_DEFAULT_GET(const char *key, const as_val *value, void *array)
{
    as_status status = AEROSPIKE_OK;
    Aerospike_object *aerospike_object = ((foreach_callback_udata *) array)->obj;
    TSRMLS_FETCH();
    AEROSPIKE_WALKER_SWITCH_CASE_GET_DEFAULT_ASSOC(((foreach_callback_udata *) array)->error_p,
            NULL, (void *) key, (void *) value, ((foreach_callback_udata *) array)->udata_p, exit);

exit:
    return (((((foreach_callback_udata *) array)->error_p)->code == AEROSPIKE_OK) ? true : false);
}

/*
 *******************************************************************************************************
 * Callback function for as_list_foreach.
 *
 * @param value                 The current value.
 * @param array                 The foreach_callback_udata struct containing the PHP list array 
 *                              as well as the as_error to be populated by the callback.
 *
 * @return true if the callback succeeds. Otherwise false.
 *******************************************************************************************************
 */
bool AS_LIST_GET_CALLBACK(as_val *value, void *array)
{
    as_status status = AEROSPIKE_OK;
    Aerospike_object *aerospike_object = ((foreach_callback_udata *) array)->obj;
    TSRMLS_FETCH();
    AEROSPIKE_WALKER_SWITCH_CASE_GET_LIST_APPEND(((foreach_callback_udata *) array)->error_p,
            NULL, NULL, (void *) value, ((foreach_callback_udata *) array)->udata_p, exit);

exit:
    return (((((foreach_callback_udata *) array)->error_p)->code == AEROSPIKE_OK) ? true : false);
}
/*
*******************************************************************************************************
* Callback function for as_map_foreach.
*
* @param key The key.
* @param value The current value.
* @param array The foreach_callback_udata struct containing the PHP map array
* as well as the as_error to be populated by the callback.
*
* @return true if the callback succeeds. Otherwise false.
*******************************************************************************************************
*/
bool AS_MAP_GET_CALLBACK(as_val *key, as_val *value, void *array)
{
    as_status status = AEROSPIKE_OK;
    Aerospike_object *aerospike_object = ((foreach_callback_udata *) array)->obj;
    TSRMLS_FETCH();
    if (FETCH_VALUE_GET(key) == AS_INTEGER) {
        AEROSPIKE_WALKER_SWITCH_CASE_GET_MAP_INDEX(((foreach_callback_udata *) array)->error_p,
                NULL, (void *) key, (void *) value, ((foreach_callback_udata *) array)->udata_p,
                exit);
    } else {
        AEROSPIKE_WALKER_SWITCH_CASE_GET_MAP_ASSOC(((foreach_callback_udata *) array)->error_p,
                NULL, (void *) key, (void *) value, ((foreach_callback_udata *) array)->udata_p,
                exit);
    }
exit:
    return (((((foreach_callback_udata *) array)->error_p)->code == AEROSPIKE_OK) ? true : false);
}

/*
*******************************************************************************************************
* End of helper functions for GET.
*******************************************************************************************************
*/

/*
 *******************************************************************************************************
 * PUT helper functions.
 *******************************************************************************************************
 */

/*
 *******************************************************************************************************
 * Misc SET calls for GET and PUT.
 *******************************************************************************************************
 */

/*
 *******************************************************************************************************
 * Appends a list to a list.
 *
 * @param outer_store       The outer as_arraylist to which inner as_list is to be
 *                          appended.
 * @param inner_store       The inner as_list to be appended to the outer as_arraylist.
 * @param bin_name          The bin name.
 * @param error_p           The as_error to be populated by the function with
 *                          encountered error if any.
 *
 *******************************************************************************************************
 */
static void AS_LIST_SET_APPEND_LIST(void* outer_store, void* inner_store,
        void* bin_name, as_error *error_p TSRMLS_DC)
{
    if (AEROSPIKE_OK != ((error_p->code) =
                as_arraylist_append_list((as_arraylist *)outer_store,
                        (as_list*) inner_store))) {
        DEBUG_PHP_EXT_DEBUG("Unable to append list to list");
        PHP_EXT_SET_AS_ERR(error_p, error_p->code,
                "Unable to append list to list");
        goto exit;
    }
    PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_OK, DEFAULT_ERROR);

exit:
    return;
}

/*
 *******************************************************************************************************
 * Appends a map to a list.
 *
 * @param outer_store       The outer as_arraylist to which as_map is to be
 *                          appended.
 * @param inner_store       The as_map to be appended to the outer as_arraylist.
 * @param bin_name          The bin name.
 * @param error_p           The as_error to be populated by the function with
 *                          encountered error if any.
 *
 *******************************************************************************************************
 */
static void AS_LIST_SET_APPEND_MAP(void* outer_store, void* inner_store,
        void* bin_name, as_error *error_p TSRMLS_DC)
{
    if (AEROSPIKE_OK != ((error_p->code) =
                as_arraylist_append_map((as_arraylist *)outer_store,
                        (as_map*) inner_store))) {
        DEBUG_PHP_EXT_DEBUG("Unable to append map to list");
        PHP_EXT_SET_AS_ERR(error_p, error_p->code,
                "Unable to append map to list");
        goto exit;
    }
    PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_OK, DEFAULT_ERROR);

exit:
    return;
}

/*
 *******************************************************************************************************
 * Sets a list in a record.
 *
 * @param outer_store       The as_record in which as_list is to be set.
 * @param inner_store       The as_list to be set in the record.
 * @param bin_name          The bin name.
 * @param error_p           The as_error to be populated by the function with
 *                          encountered error if any.
 *
 *******************************************************************************************************
 */
static void AS_DEFAULT_SET_ASSOC_LIST(void* outer_store, void* inner_store,
        void* bin_name, as_error *error_p TSRMLS_DC)
{
    if (!(as_record_set_list((as_record *)outer_store, (const char *)bin_name,
                    (as_list *) inner_store))) {
        DEBUG_PHP_EXT_DEBUG("Unable to set record to a list");
        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                "Unable to set record to a list");
        goto exit;
    }
    PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_OK, DEFAULT_ERROR);

exit:
    return;
}

/*
 *******************************************************************************************************
 * Sets a map in a record.
 *
 * @param outer_store       The as_record in which as_map is to be set.
 * @param inner_store       The as_map to be set in the record.
 * @param bin_name          The bin name.
 * @param error_p           The as_error to be populated by the function with
 *                          encountered error if any.
 *
 *******************************************************************************************************
 */
static void AS_DEFAULT_SET_ASSOC_MAP(void* outer_store, void* inner_store,
        void* bin_name, as_error *error_p TSRMLS_DC)
{
    if (!(as_record_set_map((as_record *)outer_store, (const char *)bin_name,
                    (as_map *) inner_store))) {
        DEBUG_PHP_EXT_DEBUG("Unable to set record to a map");
        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                "Unable to set record to a map");
        goto exit;
    }
    PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_OK, DEFAULT_ERROR);

exit:
    return;
}

/*
 *******************************************************************************************************
 * Sets a list in a map.
 *
 * @param outer_store       The as_hashmap in which list is to be set.
 * @param inner_store       The as_list to be set in the as_hashmap.
 * @param bin_name          The bin name.
 * @param error_p           The as_error to be populated by the function with
 *                          encountered error if any.
 *
 *******************************************************************************************************
 */
static void AS_MAP_SET_ASSOC_LIST(void* outer_store, void* inner_store,
        void* bin_name, as_error *error_p TSRMLS_DC)
{
    if (AEROSPIKE_OK != ((error_p->code) =
                as_hashmap_set((as_hashmap*)outer_store, bin_name,
                        (as_val*)((as_list *) inner_store)))) {
        DEBUG_PHP_EXT_DEBUG("Unable to set list to as_hashmap");
        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                "Unable to set list to a hashmap");
        goto exit;
    }
    PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_OK, DEFAULT_ERROR);

exit:
    return;
}

/*
 *******************************************************************************************************
 * Sets a map in a map.
 *
 * @param outer_store       The as_hashmap in which as_map is to be set.
 * @param inner_store       The as_map to be set in the as_hashmap.
 * @param bin_name          The bin name.
 * @param error_p           The as_error to be populated by the function with
 *                          encountered error if any.
 *
 *******************************************************************************************************
 */
static void AS_MAP_SET_ASSOC_MAP(void* outer_store, void* inner_store,
        void* bin_name, as_error *error_p TSRMLS_DC)
{
    if (AEROSPIKE_OK != ((error_p->code) =
                as_hashmap_set((as_hashmap*)outer_store, bin_name,
                    (as_val*)((as_map *) inner_store)))) {
        DEBUG_PHP_EXT_DEBUG("Unable to set map to as_hashmap");
        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                "Unable to set map to as_hashmap");
        goto exit;
    }
    PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_OK, DEFAULT_ERROR);

exit:
    return;
}

/* 
 *******************************************************************************************************
 * Wrappers for appeding datatype to List.
 *******************************************************************************************************
 */

static void AS_SET_ERROR_CASE(void* key, void* value, void* array,
                              void* static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC)
{
    PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR, "Error");
}

/*
 *******************************************************************************************************
 * Appends an integer to a list.
 *
 * @param key                   The key for the array.
 * @param value                 The integer value to be appended to the as_arraylist.
 * @param array                 The as_arraylist to be appended to.
 * @param static_pool           The static pool.
 * @param serializer_policy     The serializer policy for put.
 * @param error_p               The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void AS_LIST_PUT_APPEND_INT64(void* key, void *value, void *array,
        void *static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC)
{
    if (AEROSPIKE_OK != (error_p->code =
                as_arraylist_append_int64((as_arraylist *) array,
                        (int64_t)Z_LVAL_PP((zval**) value)))) {
        DEBUG_PHP_EXT_DEBUG("Unable to append integer to list");
        PHP_EXT_SET_AS_ERR(error_p, error_p->code,
                "Unable to append integer to list");
        goto exit;
    }
    PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_OK, DEFAULT_ERROR);

exit:
    return;
}
 
/*
 *******************************************************************************************************
 * Appends a string to a list.
 *
 * @param key                   The key for the array.
 * @param value                 The string value to be appended to the as_arraylist.
 * @param array                 The as_arraylist to be appended to.
 * @param static_pool           The static pool.
 * @param serializer_policy     The serializer policy for put.
 * @param error_p               The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void AS_LIST_PUT_APPEND_STR(void *key, void *value, void *array,
        void *static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC)
{
    if (AEROSPIKE_OK != (error_p->code =
                as_arraylist_append_str((as_arraylist *) array,
                        Z_STRVAL_PP((zval**) value)))) {
        DEBUG_PHP_EXT_DEBUG("Unable to append string to list");
        PHP_EXT_SET_AS_ERR(error_p, error_p->code,
                "Unable to append string to list");
        goto exit;
    }
    PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_OK, DEFAULT_ERROR);

exit:
    return;
}

/*
 *******************************************************************************************************
 * Appends a list to a list.
 *
 * @param key                   The key for the array.
 * @param value                 The list value to be appended to the as_arraylist.
 * @param array                 The as_arraylist to be appended to.
 * @param static_pool           The static pool.
 * @param serializer_policy     The serializer policy for put.
 * @param error_p               The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void AS_LIST_PUT_APPEND_LIST(void *key, void *value, void *array,
        void *static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC)
{
    AS_LIST_PUT(key, value, array, static_pool, serializer_policy, error_p TSRMLS_CC);
}

/*
 *******************************************************************************************************
 * Appends a map to a list.
 *
 * @param key                   The key for the array.
 * @param value                 The map value to be appended to the as_arraylist.
 * @param array                 The as_arraylist to be appended to.
 * @param static_pool           The static pool.
 * @param serializer_policy     The serializer policy for put.
 * @param error_p               The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void AS_LIST_PUT_APPEND_MAP(void *key, void *value, void *array,
        void *static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC)
{
    AS_MAP_PUT(key, value, array, static_pool, serializer_policy, error_p TSRMLS_CC);
}

/*
 *******************************************************************************************************
 * Wrappers for associating datatype with Record.
 *******************************************************************************************************
 */

/*
 *******************************************************************************************************
 * Sets a NIL value in a record.
 *
 * @param key                   The bin name to which nil value is to be set.
 * @param value                 The nil value to be set in the record.
 * @param array                 The as_record to which nil value is to be set.
 * @param static_pool           The static pool.
 * @param serializer_policy     The serializer policy for put.
 * @param error_p               The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void AS_DEFAULT_PUT_ASSOC_NIL(void* key, void* value, void* array,
        void* static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC)
{
    if (!as_record_set_nil((as_record *)array,
                    (const char *) key)) {
        DEBUG_PHP_EXT_DEBUG("Unable to set record to nil");
        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                "Unable to set record to nil");
        goto exit;
    }
    PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_OK, DEFAULT_ERROR);

exit:
    return;
}

/*
 *******************************************************************************************************
 * Sets a bytes value in a record.
 *
 * @param key                   The bin name to which bytes value is to be set.
 * @param value                 The zval to be serialized to a bytes value to be set in the record.
 * @param array                 The as_record to which bytes value is to be set.
 * @param static_pool           The static pool.
 * @param serializer_policy     The serializer policy for put.
 * @param error_p               The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void AS_DEFAULT_PUT_ASSOC_BYTES(void* key, void* value,
        void* array, void* static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC)
{
    as_bytes     *bytes;
    GET_BYTES_POOL(bytes, static_pool, error_p, exit);

    serialize_based_on_serializer_policy(serializer_policy, bytes,
            (zval **) value, error_p TSRMLS_CC);
    if (AEROSPIKE_OK != (error_p->code)) {
        goto exit;
    }

    if (!(as_record_set_bytes((as_record *)array, (const char *)key, bytes))) {
        DEBUG_PHP_EXT_DEBUG("Unable to set record to bytes");
        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                "Unable to set record to bytes");
        goto exit;
    }
    PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_OK, DEFAULT_ERROR);

exit:
    return;
}

/*
 *******************************************************************************************************
 * Sets an integer value in a record.
 *
 * @param key                   The bin name to which integer value is to be set.
 * @param value                 The integer value to be set in the record.
 * @param array                 The as_record to which integer value is to be set.
 * @param static_pool           The static pool.
 * @param serializer_policy     The serializer policy for put.
 * @param error_p               The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void AS_DEFAULT_PUT_ASSOC_INT64(void* key, void* value, void* array,
        void* static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC)
{
    if (!(as_record_set_int64((as_record *)array, (const char*)key,
                    (int64_t) Z_LVAL_PP((zval**) value)))) {
        DEBUG_PHP_EXT_DEBUG("Unable to set record to an int");
        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                "Unable to set record to int");
        goto exit;
    }
    PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_OK, DEFAULT_ERROR);

exit:
    return;
}

/*
 *******************************************************************************************************
 * Sets a string value in a record.
 *
 * @param key                   The bin name to which string value is to be set.
 * @param value                 The string value to be set in the record.
 * @param array                 The as_record to which string value is to be set.
 * @param static_pool           The static pool.
 * @param serializer_policy     The serializer policy for put.
 * @param error_p               The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void AS_DEFAULT_PUT_ASSOC_STR(void *key, void *value, void *array,
        void *static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC)
{
    if (!(as_record_set_str((as_record *)array, (const char*)key,
                    (char *) Z_STRVAL_PP((zval**) value)))) {
        DEBUG_PHP_EXT_DEBUG("Unable to set record to a string");
        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR,
                "Unable to set record to a string");
        goto exit;
    }
    PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_OK, DEFAULT_ERROR);

exit:
    return;
}

/*
 *******************************************************************************************************
 * Sets a list value in a record.
 *
 * @param key                   The bin name to which list value is to be set.
 * @param value                 The list value to be set in the record.
 * @param array                 The as_record to which list value is to be set.
 * @param static_pool           The static pool.
 * @param serializer_policy     The serializer policy for put.
 * @param error_p               The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void AS_DEFAULT_PUT_ASSOC_LIST(void *key, void *value, void *array,
        void *static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC)
{
    AS_LIST_PUT(key, value, array, static_pool, serializer_policy, error_p TSRMLS_CC);
}

/*
 *******************************************************************************************************
 * Sets a map value in a record.
 *
 * @param key                   The bin name to which map value is to be set.
 * @param value                 The map value to be set in the record.
 * @param array                 The as_record to which map value is to be set.
 * @param static_pool           The static pool.
 * @param serializer_policy     The serializer policy for put.
 * @param error_p               The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void AS_DEFAULT_PUT_ASSOC_MAP(void *key, void *value, void *array,
        void *static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC)
{
    AS_MAP_PUT(key, value, array, static_pool, serializer_policy, error_p TSRMLS_CC);
}

/*
 *******************************************************************************************************
 * Wrappers for associating datatype with MAP.
 *******************************************************************************************************
 */

/*
 *******************************************************************************************************
 * Sets an integer value in a map.
 *
 * @param key                   The key to be set in the as_hashmap.
 * @param value                 The integer value to be set in the as_hashmap.
 * @param store                 The as_hashmap to which integer value is to be set.
 * @param static_pool           The static pool.
 * @param serializer_policy     The serializer policy for put.
 * @param error_p               The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void AS_MAP_PUT_ASSOC_INT64(void *key, void *value, void *store,
        void *static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC)
{
    as_integer  *map_int;
    GET_INT_POOL(map_int, static_pool, error_p, exit);
    as_integer_init(map_int, Z_LVAL_PP((zval**)value));
    if (AEROSPIKE_OK != ((error_p->code) =
                as_hashmap_set((as_hashmap*)store, (as_val *) key,
                        (as_val *)(map_int)))) {
        DEBUG_PHP_EXT_DEBUG("Unable to set integer value to as_hashmap");
        PHP_EXT_SET_AS_ERR(error_p, error_p->code,
                "Unable to set integer value to as_hashmap");
    } else {
        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_OK, DEFAULT_ERROR);
    }
exit:
    return;
}

/*
 *******************************************************************************************************
 * Sets a string value in a map.
 *
 * @param key                   The key to be set in the as_hashmap.
 * @param value                 The string value to be set in the as_hashmap.
 * @param store                 The as_hashmap to which string value is to be set.
 * @param static_pool           The static pool.
 * @param serializer_policy     The serializer policy for put.
 * @param error_p               The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void AS_MAP_PUT_ASSOC_STR(void *key, void *value, void *store,
        void *static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC)
{
    as_string   *map_str;
    GET_STR_POOL(map_str, static_pool, error_p, exit);
    as_string_init(map_str, Z_STRVAL_PP((zval**)value), false);
    if (AEROSPIKE_OK != ((error_p->code) =
                as_hashmap_set((as_hashmap*)store, (as_val *) key,
                        (as_val *)(map_str)))) {
        DEBUG_PHP_EXT_DEBUG("Unable to set string value to as_hashmap");
        PHP_EXT_SET_AS_ERR(error_p, error_p->code,
                "Unable to set string value to as_hashmap");
    } else {
        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_OK, DEFAULT_ERROR);
    }
exit:
    return;
}

/*
 *******************************************************************************************************
 * Sets a map value in a map.
 *
 * @param key                   The key to be set in the as_hashmap.
 * @param value                 The as_map value to be set in the as_hashmap.
 * @param store                 The as_hashmap to which map value is to be set.
 * @param static_pool           The static pool.
 * @param serializer_policy     The serializer policy for put.
 * @param error_p               The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void AS_MAP_PUT_ASSOC_MAP(void *key, void *value, void *store,
        void *static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC)
{
    AS_MAP_PUT(key, value, store, static_pool, serializer_policy, error_p TSRMLS_CC);
}

/*
 *******************************************************************************************************
 * Sets a list value in a map.
 *
 * @param key                   The key to be set in the as_hashmap.
 * @param value                 The as_list value to be set in the as_hashmap.
 * @param store                 The as_hashmap to which list value is to be set.
 * @param static_pool           The static pool.
 * @param serializer_policy     The serializer policy for put.
 * @param error_p               The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void AS_MAP_PUT_ASSOC_LIST(void *key, void *value, void *store,
        void *static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC)
{
    AS_LIST_PUT(key, value, store, static_pool, serializer_policy, error_p TSRMLS_CC);
}

/*
 *******************************************************************************************************
 * Sets a bytes value in a map.
 *
 * @param key                   The key to be set in the as_hashmap.
 * @param value                 The zval to be serialized into as_bytes value to be set in the as_hashmap.
 * @param store                 The as_hashmap to which bytes value is to be set.
 * @param static_pool           The static pool.
 * @param serializer_policy     The serializer policy for put.
 * @param error_p               The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void AS_MAP_PUT_ASSOC_BYTES(void *key, void *value, void *store,
        void *static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC)
{
    as_bytes     *bytes;
    GET_BYTES_POOL(bytes, static_pool, error_p, exit);
    serialize_based_on_serializer_policy(serializer_policy, bytes,
            (zval **) value, error_p TSRMLS_CC);
    if (AEROSPIKE_OK != (error_p->code)) {
        goto exit;
    }

    if (AEROSPIKE_OK != ((error_p->code) =
                as_hashmap_set((as_hashmap*)store, (as_val *) key,
                        (as_val *)(bytes)))) {
        DEBUG_PHP_EXT_DEBUG("Unable to set byte value to as_hashmap");
        PHP_EXT_SET_AS_ERR(error_p, error_p->code,
                "Unable to set string value to as_hashmap");
    } else {
        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_OK, DEFAULT_ERROR);
    }

exit:
    return;
}

static void AS_DEFAULT_PUT_ASSOC_ARRAY(void *key, void *value, void *store,
        void *static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC);
static void AS_MAP_PUT_ASSOC_ARRAY(void *key, void *value, void *store,
        void *static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC);
static void AS_LIST_PUT_APPEND_ARRAY(void *key, void *value, void *store,
        void *static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC);
static void AS_DEFAULT_PUT_ASSOC_BYTES(void *key, void *value, void *store,
        void *static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC);
static void AS_MAP_PUT_ASSOC_BYTES(void *key, void *value, void *store,
        void *static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC);
static void AS_LIST_PUT_APPEND_BYTES(void *key, void *value, void *array,
        void *static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC);

/*
 *******************************************************************************************************
 * PUT functions whose macros will expand.
 *******************************************************************************************************
 */

/*
 *******************************************************************************************************
 * Puts a value in an as_record.
 *
 * @param key                   The bin name for the record.
 * @param value                 The value to be put in the as_record.
 * @param record_p              The as_record in which value is to be put.
 * @param static_pool           The static pool.
 * @param serializer_policy     The serializer policy for put.
 * @param error_p               The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
void AS_DEFAULT_PUT(void *key, void *value, as_record *record_p, void *static_pool,
        uint32_t serializer_policy, as_error *error_p TSRMLS_DC)
{
    AEROSPIKE_WALKER_SWITCH_CASE_PUT_DEFAULT_ASSOC(error_p, static_pool,
            key, ((zval**)value), record_p, exit, serializer_policy);
exit:
    return;
}

/*
 *******************************************************************************************************
 * Puts a value in an as_list.
 *
 * @param key                   The key for the array (NULL).
 * @param value                 The value to be put in the as_list.
 * @param store                 The as_list in which value is to be put.
 * @param static_pool           The static pool.
 * @param serializer_policy     The serializer policy for put.
 * @param error_p               The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
extern void AS_LIST_PUT(void *key, void *value, void *store, void *static_pool,
        uint32_t serializer_policy, as_error *error_p TSRMLS_DC)
{
    AEROSPIKE_WALKER_SWITCH_CASE_PUT_LIST_APPEND(error_p, static_pool,
            key, ((zval**)value), store, exit, serializer_policy);
exit:
    return;
}

/*
 *******************************************************************************************************
 * Puts a value in an as_map.
 *
 * @param key                   The key for the map.
 * @param value                 The value to be put in the as_map.
 * @param store                 The as_map in which value is to be put.
 * @param static_pool           The static pool.
 * @param serializer_policy     The serializer policy for put.
 * @param error_p               The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
void AS_MAP_PUT(void *key, void *value, void *store, void *static_pool,
        uint32_t serializer_policy, as_error *error_p TSRMLS_DC)
{
    as_val *map_key = NULL;
    AEROSPIKE_WALKER_SWITCH_CASE_PUT_MAP_ASSOC(error_p, static_pool,
            map_key, ((zval**)value), store, exit, serializer_policy);
exit:
    return;
}

/*
 *******************************************************************************************************
 * Puts an array value in an as_record.
 *
 * @param key                   The bin name.
 * @param value                 The array value to be put in the as_record.
 * @param store                 The as_record in which value is to be put.
 * @param static_pool           The static pool.
 * @param serializer_policy     The serializer policy for put.
 * @param error_p               The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void AS_DEFAULT_PUT_ASSOC_ARRAY(void *key, void *value, void *store,
        void *static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC)
{
    AEROSPIKE_PROCESS_ARRAY(DEFAULT, ASSOC, exit, key, value, store,
            error_p, static_pool, serializer_policy);
exit:
    return;
}

/*
 *******************************************************************************************************
 * Puts an array value in an as_map.
 *
 * @param key                   The bin name.
 * @param value                 The array value to be put in the as_map.
 * @param store                 The as_map in which value is to be put.
 * @param static_pool           The static pool.
 * @param serializer_policy     The serializer policy for put.
 * @param error_p               The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void AS_MAP_PUT_ASSOC_ARRAY(void *key, void *value, void *store,
        void *static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC)
{
    AEROSPIKE_PROCESS_ARRAY(MAP, ASSOC, exit, key, value, store,
            error_p, static_pool, serializer_policy);
exit:
    return;
}

/*
 *******************************************************************************************************
 * Puts an array value in an as_list.
 *
 * @param key                   The bin name.
 * @param value                 The array value to be put in the as_list.
 * @param store                 The as_list in which value is to be put.
 * @param static_pool           The static pool.
 * @param serializer_policy     The serializer policy for put.
 * @param error_p               The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void AS_LIST_PUT_APPEND_ARRAY(void *key, void *value, void *store,
        void *static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC)
{
    AEROSPIKE_PROCESS_ARRAY(LIST, APPEND, exit, key, value, store,
            error_p, static_pool, serializer_policy);
exit:
    return;
}

/*
 *******************************************************************************************************
 * Appends bytes value to an as_list.
 *
 * @param key                   The key for the array (NULL).
 * @param value                 The zval to be serialized into as_bytes value to be appended to the as_list.
 * @param array                 The as_list to which value is to be appended to.
 * @param static_pool           The static pool.
 * @param serializer_policy     The serializer policy for put.
 * @param error_p               The as_error to be populated by the function with
 *                              encountered error if any.
 *
 *******************************************************************************************************
 */
static void AS_LIST_PUT_APPEND_BYTES(void* key, void *value, void *array,
        void *static_pool, uint32_t serializer_policy, as_error *error_p TSRMLS_DC)
{
    as_bytes     *bytes;
    GET_BYTES_POOL(bytes, static_pool, error_p, exit);

    serialize_based_on_serializer_policy(serializer_policy, bytes,
            (zval **) value, error_p TSRMLS_CC);
    if(AEROSPIKE_OK != (error_p->code)) {
        goto exit;
    }

    if (AEROSPIKE_OK != ((error_p->code) =
                as_arraylist_append_bytes((as_arraylist *) array, bytes))) {
        DEBUG_PHP_EXT_DEBUG("Unable to append integer to list");
        PHP_EXT_SET_AS_ERR(error_p, error_p->code,
                "Unable to append integer to list");
        goto exit;
    }
    PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_OK, DEFAULT_ERROR);

exit:
    return;
}

/* 
 *******************************************************************************************************
 * End of PUT helper functions. 
 *******************************************************************************************************
 */

typedef as_status (*aerospike_transform_key_callback)(HashTable* ht_p,
                                                      u_int32_t key_data_type_u32, 
                                                      int8_t* key_p, u_int32_t key_len_u32,
                                                      void* data_p, zval** retdata_pp);

/* 
 *******************************************************************************************************
 * Structure for expected input key in put from PHP userland.
 *******************************************************************************************************
 */
typedef struct asputkeydatamap {
    int8_t* namespace_p;
    int8_t* set_p;
    zval**  key_pp;
    int     is_digest;
} as_put_key_data_map;

/* 
 *******************************************************************************************************
 * Structure for asconfig iterator.
 *******************************************************************************************************
 */
typedef struct asconfig_iter {
    u_int32_t     iter_count_u32;
    as_config*    as_config_p;
} as_config_iter_map;

/* 
 *******************************************************************************************************
 * Union for addrport iterator.
 * Holds either as_config or a string.
 *******************************************************************************************************
 */
typedef union _addrport_callback_result {
    as_config* as_config_p;
    char*      ip_port_p;
} addrport_callback_result_t;

/* 
 *******************************************************************************************************
 * Structure for config transform iterator.
 * Used for iterating over addr and port and transforming it into either
 * as_config or a string ip_port.
 *******************************************************************************************************
 */
typedef struct addrport_transform_iter {
    u_int32_t                               iter_count_u32;
    enum config_transform_result_type       transform_result_type;
    addrport_callback_result_t              transform_result;
} addrport_transform_iter_map_t;

/* 
 *******************************************************************************************************
 * Structure for config transform iterator.
 * Used for iterating over config array and transforming it into either
 * as_config or a zval.
 *******************************************************************************************************
 */
typedef struct config_transform_iter {
    u_int32_t                               iter_count_u32;
    enum config_transform_result_type       transform_result_type;
    transform_result_t                      transform_result;
} config_transform_iter_map_t;

#define AS_CONFIG_ITER_MAP_SET_ADDR(map_p, val)                                                 \
do {                                                                                            \
    map_p->transform_result.as_config_p->hosts[map_p->iter_count_u32].addr = val;               \
    map_p->transform_result.as_config_p->hosts_size++;                                          \
} while(0)    

#define AS_CONFIG_ITER_MAP_SET_PORT(map_p, val)  map_p->transform_result.as_config_p->hosts[map_p->iter_count_u32].port = val
#define AS_CONFIG_SET_USER(as_config_p, val)     strncpy(as_config_p->user, val, strlen(val))
#define AS_CONFIG_SET_PASSWORD(as_config_p, val) strncpy(as_config_p->password, val, strlen(val))
#define AS_CONFIG_ITER_MAP_IS_ADDR_SET(map_p)    (map_p->transform_result.as_config_p->hosts[map_p->iter_count_u32].addr)
#define AS_CONFIG_ITER_MAP_IS_PORT_SET(map_p)    (map_p->transform_result.as_config_p->hosts[map_p->iter_count_u32].port)

/* 
 *******************************************************************************************************
 * Set input user in as_config.
 *
 * @param user_p        The string containing username to be set into as_config.
 * @param config_p      The c-client's as_config object to be set.
 *
 * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_x.
 *******************************************************************************************************
 */
static as_status
aerospike_transform_set_user_in_config(char *user_p, as_config *config_p)
{
    as_status      status = AEROSPIKE_OK;
    
    if (!user_p || !config_p) {
        status = AEROSPIKE_ERR;
        goto exit;
    }
    /*
     * TODO: Uncomment below line to set the user_p in config_p
     */
    /*AS_CONFIG_SET_USER(config_p, user_p);*/

exit:
    return status;
}

/* 
 *******************************************************************************************************
 * Set input password in as_config.
 *
 * @param password_p        The string containing password to be set into as_config.
 * @param config_p          The c-client's as_config object to be set.
 *
 * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_x.
 *******************************************************************************************************
 */
static as_status
aerospike_transform_set_password_in_config(char *password_p, as_config *config_p)
{
    as_status      status = AEROSPIKE_OK;
    
    if (!password_p || !config_p) {
        status = AEROSPIKE_ERR;
        goto exit;
    }
    /*
     * TODO: Uncomment below line to set the password_p in config_p
     */
    /*AS_CONFIG_SET_PASSWORD(config_p, password_p);*/

exit:
    return status;
}

/* 
 *******************************************************************************************************
 * This function iterates over the keys of a PHP array and performs the
 * required checks with the help of the given keycallback function.
 *
 * @param ht_p              The hashtable pointing to the PHP array.
 * @param retdata_pp        The return zval to be poplated with the data within
 *                          the PHP array.
 * @keycallback_p           The callback function that shall perform the
 *                          required checks on each key of the given PHP array.
 * @data_p                  User data to be passed into the keycallback.
 *
 * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_x.
 *******************************************************************************************************
 */
static as_status 
aerospike_transform_iterateKey(HashTable* ht_p, zval** retdata_pp, 
                               aerospike_transform_key_callback keycallback_p,
                               void* data_p)
{
    as_status        status = AEROSPIKE_OK;
    HashPosition     hashPosition_p = NULL;
    zval**           keyData_pp = NULL;

    foreach_hashtable(ht_p, hashPosition_p, keyData_pp) {
        int8_t*     key_value_p = NULL;
        u_int32_t   key_len_u32 = 0;
        ulong       index_u64 = 0;
        u_int32_t   key_type_u32 = zend_hash_get_current_key_ex(ht_p, 
                                                                (char **)&key_value_p,
                                                                &key_len_u32,
                                                                &index_u64, 0,
                                                                &hashPosition_p);

        /* check for key type , need to know what it is*/

        if(keycallback_p) {
            if (AEROSPIKE_OK != (status = keycallback_p(ht_p,
                                                        Z_TYPE_PP(keyData_pp),
                                                        key_value_p, key_len_u32,
                                                        data_p, keyData_pp))) {
                goto exit;
            }
        }
    }
exit:

    if ((retdata_pp) && (keyData_pp)) {
        *retdata_pp = *keyData_pp;
    }

    return status;
}

/* 
 *******************************************************************************************************
 * Callback for checking expected keys (hosts, user and password) in the input
 * config array for Aerospike::construct().
 *
 * @param ht_p                      The hashtable pointing to the input PHP config array.
 * @param key_data_type_u32         The key datatype of current key in the config array.
 * @param key_p                     The current key in the config array.
 * @param key_len_u32               The length of the current key.
 * @param data_p                    The struct containing (as_config/zval) to be set
 *                                  within the callback.
 * @param value_pp                  The zval value for current key.
 *
 * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_x.
 *******************************************************************************************************
 */
static as_status
aerospike_transform_config_callback(HashTable* ht_p,
                                    u_int32_t key_data_type_u32,
                                    int8_t* key_p, u_int32_t key_len_u32,
                                    void* data_p, zval** value_pp)
{
    as_status      status = AEROSPIKE_OK;
    TSRMLS_FETCH();
    if (PHP_IS_ARRAY(key_data_type_u32) && 
        PHP_COMPARE_KEY(PHP_AS_KEY_DEFINE_FOR_HOSTS,
            PHP_AS_KEY_DEFINE_FOR_HOSTS_LEN, key_p, key_len_u32 - 1)) {
            status = aerospike_transform_iteratefor_addr_port(Z_ARRVAL_PP(value_pp),
                    data_p);
    } else if (PHP_IS_STRING(key_data_type_u32) && 
        PHP_COMPARE_KEY(PHP_AS_KEY_DEFINE_FOR_USER, PHP_AS_KEY_DEFINE_FOR_USER_LEN,
            key_p, key_len_u32 -1)) {
            if (((transform_zval_config_into *) data_p)->transform_result_type == TRANSFORM_INTO_AS_CONFIG) {
                status = aerospike_transform_set_user_in_config(Z_STRVAL_PP(value_pp),
                        (((transform_zval_config_into *) data_p)->transform_result).as_config_p);
            } else {
                DEBUG_PHP_EXT_DEBUG("Skipping users as zval config is to be transformed into host_lookup");
                status = AEROSPIKE_OK;
            }
    } else if (PHP_IS_STRING(key_data_type_u32) && 
        PHP_COMPARE_KEY(PHP_AS_KEY_DEFINE_FOR_PASSWORD,
            PHP_AS_KEY_DEFINE_FOR_PASSWORD_LEN, key_p, key_len_u32 -1)) {
            if (((transform_zval_config_into *) data_p)->transform_result_type == TRANSFORM_INTO_AS_CONFIG) {
                status = aerospike_transform_set_password_in_config(Z_STRVAL_PP(value_pp),
                        (((transform_zval_config_into *) data_p)->transform_result).as_config_p);
            } else {
                DEBUG_PHP_EXT_DEBUG("Skipping password as zval config is to be transformed into host_lookup");
                status = AEROSPIKE_OK;
            }
    } else {
        status = AEROSPIKE_ERR_PARAM;
        goto exit;
    }

exit:
    return status;
}

/* 
 *******************************************************************************************************
 * Check input PHP config array and translate it to corresponding as_config of
 * the c-client.
 *
 * @param ht_p                      The hashtable pointing to the input PHP config array.
 * @param retdata_pp                The return zval to be populated by the function.
 * @param config_p                  The c client's as_config to be set.
 *
 * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_x.
 *******************************************************************************************************
 */
extern as_status
aerospike_transform_check_and_set_config(HashTable* ht_p, zval** retdata_pp, /*as_config **/void* config_p)
{
    as_status      status = AEROSPIKE_OK;

    if (!ht_p) {
        status = AEROSPIKE_ERR;
        goto exit;
    }

    if (AEROSPIKE_OK != (status =
                aerospike_transform_iterateKey(ht_p, NULL/*retdata_pp*/, 
                        &aerospike_transform_config_callback,
                                config_p))) {
        goto exit;
    }

exit:
    return status;
}

/*
 *******************************************************************************************************
 * Callback for checking expected keys (addr and port) in the current host 
 * within the array of hosts from input config array for Aerospike::construct().
 *
 * @param ht_p                      The hashtable pointing to the input host array.
 * @param key_data_type_u32         The key datatype of the current key in host array.
 * @param key_p                     The current key.
 * @param key_len_u32               The length of current key.
 * @param data_p                    The user data to be passed to the callback.
 * @param value_pp                  The zval containing the current value.
 *
 * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_x.
 *******************************************************************************************************
 */
static
as_status aerospike_transform_addrport_callback(HashTable* ht_p,
                                                u_int32_t key_data_type_u32,
                                                int8_t* key_p, u_int32_t key_len_u32,
                                                void* data_p, zval** value_pp)
{
    as_status                           status = AEROSPIKE_OK;
    int                                 port = -1;
    addrport_transform_iter_map_t*      addrport_transform_iter_map_p = (addrport_transform_iter_map_t *)(data_p);
    bool                                set_as_config = false;

    if (addrport_transform_iter_map_p->transform_result_type == TRANSFORM_INTO_AS_CONFIG) {
        set_as_config = true;
    } else if (addrport_transform_iter_map_p->transform_result_type == TRANSFORM_INTO_ZVAL) {
        set_as_config = false;
    } else {
        status = AEROSPIKE_ERR;
        goto exit;
    }

    if ((!addrport_transform_iter_map_p) || (!value_pp) ||
            (set_as_config && (!addrport_transform_iter_map_p->transform_result.as_config_p)) ||
            (set_as_config && (!addrport_transform_iter_map_p->transform_result.ip_port_p))) {
        status = AEROSPIKE_ERR;
        goto exit;
    }

    if (PHP_IS_STRING(key_data_type_u32) &&
        PHP_COMPARE_KEY(PHP_AS_KEY_DEFINE_FOR_ADDR,
            PHP_AS_KEY_DEFINE_FOR_ADDR_LEN, key_p, key_len_u32 - 1)) {
        if (set_as_config) {
            AS_CONFIG_ITER_MAP_SET_ADDR(addrport_transform_iter_map_p,
                    pestrndup(Z_STRVAL_PP(value_pp), Z_STRLEN_PP(value_pp), 1));
        } else {
            strncpy(addrport_transform_iter_map_p->transform_result.ip_port_p,
                    Z_STRVAL_PP(value_pp), strlen(Z_STRVAL_PP(value_pp)) + 1);
        }
    } else if (PHP_IS_LONG(key_data_type_u32) &&
             PHP_COMPARE_KEY(PHP_AS_KEY_DEFINE_FOR_PORT,
                 PHP_AS_KEY_DEFINE_FOR_PORT_LEN, key_p, key_len_u32 - 1)) {
        if (set_as_config) {
            AS_CONFIG_ITER_MAP_SET_PORT(addrport_transform_iter_map_p, Z_LVAL_PP(value_pp));
        } else {
            snprintf(addrport_transform_iter_map_p->transform_result.ip_port_p +
                    strlen(addrport_transform_iter_map_p->transform_result.ip_port_p),
                    IP_PORT_MAX_LEN, ":%d", Z_LVAL_PP(value_pp));
        }
    } else if (PHP_IS_STRING(key_data_type_u32) &&
             PHP_COMPARE_KEY(PHP_AS_KEY_DEFINE_FOR_PORT,
                 PHP_AS_KEY_DEFINE_FOR_PORT_LEN, key_p, key_len_u32 - 1)) {
        port = atoi(Z_STRVAL_PP(value_pp));
        if (port < 0 || port > 65535) {
            status = AEROSPIKE_ERR_PARAM;
            goto exit;
        }
        if (set_as_config) {
            AS_CONFIG_ITER_MAP_SET_PORT(addrport_transform_iter_map_p, port);
        } else {
            snprintf(addrport_transform_iter_map_p->transform_result.ip_port_p +
                    strlen(addrport_transform_iter_map_p->transform_result.ip_port_p),
                    IP_PORT_MAX_LEN, ":%s", Z_STRVAL_PP(value_pp));
        }
    } else {
        status = AEROSPIKE_ERR_PARAM;
        goto exit;
    }

exit:
    return status;
}

/* 
 *******************************************************************************************************
 * Callback for checking expected keys (addr and port) in each host within the array of hosts
 * in the input config array for Aerospike::construct().
 *
 * @param ht_p                      The hashtable pointing to the input array of hosts.
 * @param key_data_type_u32         The key datatype of the current key in host array.
 * @param key_p                     The current key.
 * @param key_len_u32               The length of current key.
 * @param data_p                    The user data to be passed to the callback.
 * @param retdata_pp                The return zval to be populated by the function.
 *
 * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_x.
 *******************************************************************************************************
 */
static
as_status aerospike_transform_array_callback(HashTable* ht_p,
                                             u_int32_t key_data_type_u32,
                                             int8_t* key_p, u_int32_t key_len_u32,
                                             void* data_p, zval** retdata_pp)
{
    as_status                               status = AEROSPIKE_OK;
    zval**                                  addrport_data_pp = NULL;
    char                                    ip_port[IP_PORT_MAX_LEN];
    addrport_transform_iter_map_t           addrport_transform_iter_map_p;
    bool                                    set_as_config = false;
    
    if (PHP_IS_NOT_ARRAY(key_data_type_u32) || (!data_p) || (!retdata_pp)) {
        status = AEROSPIKE_ERR;
        goto exit;
    }

    addrport_transform_iter_map_p.iter_count_u32 = ((config_transform_iter_map_t *) data_p)->iter_count_u32;
    addrport_transform_iter_map_p.transform_result_type = ((config_transform_iter_map_t *) data_p)->transform_result_type;

    if (addrport_transform_iter_map_p.transform_result_type == TRANSFORM_INTO_AS_CONFIG) {
        set_as_config = true;
        addrport_transform_iter_map_p.transform_result.as_config_p = (((config_transform_iter_map_t *) data_p)->transform_result).as_config_p;
    } else if (addrport_transform_iter_map_p.transform_result_type == TRANSFORM_INTO_ZVAL) {
        set_as_config = false;
        addrport_transform_iter_map_p.transform_result.ip_port_p = ip_port;
    } else {
        status = AEROSPIKE_ERR;
        goto exit;
    }

    if (AEROSPIKE_OK != (status = aerospike_transform_iterateKey(Z_ARRVAL_PP(retdata_pp),
                                                                 addrport_data_pp,
                                                                 &aerospike_transform_addrport_callback,
                                                                 (void *) &addrport_transform_iter_map_p))) {
        goto exit;
    }

    if (set_as_config &&
            ((!AS_CONFIG_ITER_MAP_IS_ADDR_SET(((config_transform_iter_map_t *) data_p))) &&
             (!AS_CONFIG_ITER_MAP_IS_PORT_SET(((config_transform_iter_map_t *) data_p))))) {
        /* address and port are not set so give error */
        status = AEROSPIKE_ERR_PARAM;
        goto exit;
    }
    
    if (!set_as_config) {
        zval **tmp;
        if (FAILURE == zend_hash_find((((config_transform_iter_map_t *) data_p)->transform_result).host_lookup_p,
                    ip_port, strlen(ip_port), (void**)&tmp)) {
            if (0 != zend_hash_add((((config_transform_iter_map_t *) data_p)->transform_result).host_lookup_p,
                        ip_port, strlen(ip_port), (void *) ip_port, strlen(ip_port), NULL)) {
                status = AEROSPIKE_ERR;
                goto exit;
            }
        }
    }

    ((config_transform_iter_map_t *) data_p)->iter_count_u32++;

exit:
    return status;
}

/*
 *******************************************************************************************************
 * Iterates over the hosts array and sets the host keys (addr and port) in C client's as_config.
 *
 * @param ht_p                      The hashtable pointing to the hosts array.
 * @param data_p                    The C SDK's as_config to be set using the hosts array.
 *
 * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_x.
 *******************************************************************************************************
 */
static as_status
aerospike_transform_iteratefor_addr_port(HashTable* ht_p, void* data_p)
{
    as_status                       status = AEROSPIKE_OK;
    config_transform_iter_map_t     config_transform_iter_map;
    
    config_transform_iter_map.iter_count_u32 = 0;

    if ((!ht_p) || (!data_p)) {
        status = AEROSPIKE_ERR;
        goto exit;
    }

    if (((transform_zval_config_into *) data_p)->transform_result_type == TRANSFORM_INTO_AS_CONFIG) {
        config_transform_iter_map.transform_result_type = TRANSFORM_INTO_AS_CONFIG;
        config_transform_iter_map.transform_result.as_config_p = (((transform_zval_config_into *) data_p)->transform_result).as_config_p;

    } else if (((transform_zval_config_into *) data_p)->transform_result_type == TRANSFORM_INTO_ZVAL) {
        config_transform_iter_map.transform_result_type = TRANSFORM_INTO_ZVAL;
        config_transform_iter_map.transform_result.host_lookup_p  = (((transform_zval_config_into *) data_p)->transform_result).host_lookup_p;
    }

    if (AEROSPIKE_OK != (status = aerospike_transform_iterateKey(ht_p, NULL/*retdata_pp*/,
                                                                 &aerospike_transform_array_callback,
                                                                 (void *) &config_transform_iter_map))) {
        goto exit;
    }

    if (0 == config_transform_iter_map.iter_count_u32) {
        status = AEROSPIKE_ERR;
        goto exit;
    }

exit:
    return status;
}

/* 
 *******************************************************************************************************
 * Checks and sets the as_key using the input ns, set and key value.
 *
 * @param as_key_p                  The C client's as_key to be set.
 * @param key_type                  The datatype of current key.
 * @param namespace_p               The namespace name for the record.
 * @param set_p                     The set name for the record.
 * @param key_pp                    The value of the key to be set.
 *
 * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_x.
 *******************************************************************************************************
 */
static as_status
aerospike_add_key_params(as_key* as_key_p, u_int32_t key_type, const char* namespace_p, const char* set_p, zval** key_pp, int is_digest)
{
    as_status      status = AEROSPIKE_OK;

    if ((!as_key_p) || (!namespace_p) || (!set_p) || (!key_pp)) {
        status = AEROSPIKE_ERR;
        goto exit;
    }

    switch (key_type) {
        case IS_LONG:
            if (!is_digest) {
                as_key_init_int64(as_key_p, namespace_p, set_p, (int64_t) Z_LVAL_PP(key_pp));
            } else {
                as_digest_value digest = {0};
                snprintf((char *) digest, AS_DIGEST_VALUE_SIZE, "%d", Z_LVAL_PP(key_pp));
                as_key_init_digest(as_key_p, namespace_p, set_p, digest);
            }
            break;
        case IS_STRING:
            if (!is_digest) {
                as_key_init_str(as_key_p, namespace_p, set_p, Z_STRVAL_PP(key_pp));
            } else {
                if (strlen(Z_STRVAL_PP(key_pp))) {
                    as_digest_value digest = {0};
                    strncpy((char *) digest, Z_STRVAL_PP(key_pp), strlen(Z_STRVAL_PP(key_pp)));
                    as_key_init_digest(as_key_p, namespace_p, set_p, digest);
                } else {
                    status = AEROSPIKE_ERR;
                    goto exit;
                }
            }
            break;
        default:
            status = AEROSPIKE_ERR;
            break;
    }

exit:
    return status;
}

/* 
 *******************************************************************************************************
 * Callback for checking and setting the as_key for the record to be read/written from/to Aerospike.
 *
 * @param ht_p                      The hashtable pointing to the current element of 
 *                                  input key from PHP user.
 * @param key_data_type_u32         The datatype of current key.
 * @param key_p                     The current key.
 * @param key_len_u32               The length of current key.
 * @param data_p                    The userdata for the callback.
 * @param retdata_pp                The return zval to be populated by the callback.
 *
 * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_x.
 *******************************************************************************************************
 */
static as_status
aerospike_transform_putkey_callback(HashTable* ht_p,
                                    u_int32_t key_data_type_u32,
                                    int8_t* key_p, u_int32_t key_len_u32,
                                    void* data_p, zval** retdata_pp)
{
    as_status                 status = AEROSPIKE_OK;
    as_put_key_data_map*      as_put_key_data_map_p = (as_put_key_data_map *)(data_p);

    if (!as_put_key_data_map_p) {
        status = AEROSPIKE_ERR;
        goto exit;
    }

    if (PHP_IS_STRING(key_data_type_u32) &&
        PHP_COMPARE_KEY(PHP_AS_KEY_DEFINE_FOR_NS, PHP_AS_KEY_DEFINE_FOR_NS_LEN, key_p, key_len_u32 - 1)) {
        as_put_key_data_map_p->namespace_p = (int8_t*) Z_STRVAL_PP(retdata_pp);
    } else if(PHP_IS_STRING(key_data_type_u32) &&
             PHP_COMPARE_KEY(PHP_AS_KEY_DEFINE_FOR_SET, PHP_AS_KEY_DEFINE_FOR_SET_LEN, key_p, key_len_u32 - 1)) {
        as_put_key_data_map_p->set_p = (int8_t*) Z_STRVAL_PP(retdata_pp);
    } else if(/*PHP_IS_STRING(key_data_type_u32) &&  -- need to check on the type*/
             PHP_COMPARE_KEY(PHP_AS_KEY_DEFINE_FOR_KEY, PHP_AS_KEY_DEFINE_FOR_KEY_LEN, key_p, key_len_u32 - 1)) {
        as_put_key_data_map_p->key_pp = retdata_pp;
        as_put_key_data_map_p->is_digest = 0;
    } else if(PHP_COMPARE_KEY(PHP_AS_KEY_DEFINE_FOR_DIGEST, PHP_AS_KEY_DEFINE_FOR_DIGEST_LEN, key_p, key_len_u32 - 1)) {
        as_put_key_data_map_p->key_pp = retdata_pp;
        as_put_key_data_map_p->is_digest = 1;
    }else {
        status = AEROSPIKE_ERR_PARAM;
        goto exit;
    }

exit:
    return status;
}

/* 
 *******************************************************************************************************
 * Check and set the as_key for the record to be read/written from/to Aerospike.
 *
 * @param ht_p                      The hashtable pointing to the input key from PHP user.
 * @param as_key_p                  The C client's as_key to be set.
 * @param set_val_p                 The flag to be set if as_key is allocated memory 
 *                                  so that it can be destroyed by calling function if set.
 *
 * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_x.
 *******************************************************************************************************
 */
extern as_status
aerospike_transform_iterate_for_rec_key_params(HashTable* ht_p, as_key* as_key_p, int16_t *set_val_p)
{
    as_status            status = AEROSPIKE_OK;
    as_put_key_data_map  put_key_data_map = {0};
    HashPosition         hashPosition_p = NULL;
    zval*                key_record_p = NULL;
    u_int64_t            index_u64 = 0;

    if ((!ht_p) || (!as_key_p) || (!as_key_p) || (!set_val_p)) {
        status = AEROSPIKE_ERR;
        goto exit;
    }

    foreach_hashtable(ht_p, hashPosition_p, index_u64) {
        if (AEROSPIKE_OK != (status = aerospike_transform_iterateKey(ht_p, &key_record_p,
                                                                     &aerospike_transform_putkey_callback,
                                                                     (void *)&put_key_data_map))) {
            goto exit;
        }
    }

    if (!(put_key_data_map.namespace_p) || !(put_key_data_map.set_p) || !(put_key_data_map.key_pp) || (!key_record_p)) {
        status = AEROSPIKE_ERR;
        goto exit;
    }

    if(AEROSPIKE_OK != (status = aerospike_add_key_params(as_key_p,
                                                          Z_TYPE_P(key_record_p),
                                                          (const char*) put_key_data_map.namespace_p,
                                                          (const char*) put_key_data_map.set_p,
                                                          put_key_data_map.key_pp/*&key_record_p*/,
                                                          put_key_data_map.is_digest))) {
        goto exit;
    }

    *set_val_p = 1;

exit:
    return status;
}

/* 
 *******************************************************************************************************
 * Iterate over the input PHP record array and translate it to corresponding C
 * client's as_record by transforming the datatypes from PHP to C client's
 * datatypes recursively at all possible nested levels within the record.
 *
 * @param record_pp                 The PHP user's record array.
 * @param as_record_p               The C client's as_record to be set.
 * @param static_pool               The static pool of C client datatypes (like
 *                                  as_hashmap, as_arraylist, as_string, as_integer, as_bytes)
 * @param serializer_policy         The serializer policy for writing unsupported datatypes to Aerospike. 
 * @param error_p                   The as_error object to be set with the
 *                                  encountered error or success.
 *
 * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_x.
 *******************************************************************************************************
 */
static void
aerospike_transform_iterate_records(zval **record_pp,
                                    as_record* as_record_p,
                                    as_static_pool* static_pool,
                                    uint32_t serializer_policy,
                                    as_error *error_p TSRMLS_DC)
{
    char*              key = NULL;

    if ((!record_pp) || !(as_record_p) || !(static_pool)) {
        DEBUG_PHP_EXT_DEBUG("Unable to put record");
        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR, "Unable to put record");
        goto exit;
    }

    /* switch case statements for put for zend related data types */
    AS_DEFAULT_PUT(key, record_pp, as_record_p, static_pool, serializer_policy, error_p TSRMLS_CC);

exit:
    return;
}

/* 
 *******************************************************************************************************
 * Creates and puts the as_record into Aerospike db by using appropriate write policy.
 *
 * @param as_object_p               The C client's aerospike object for the db to be written to.
 * @param record_pp                 The record to be written.
 * @param as_key_p                  The C client's as_key identifying the record to be written to. 
 * @param error_p                   The C client's as_error to be set to the encountered error.
 * @param ttl_u64                   The ttl to be set for C client's as_record.
 * @param options_p                 The optional parameters to Aerospike::put()
 *
 * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_x.
 *******************************************************************************************************
 */
extern as_status
aerospike_transform_key_data_put(aerospike* as_object_p,
                                 zval **record_pp,
                                 as_key* as_key_p,
                                 as_error *error_p,
                                 u_int32_t ttl_u32,
                                 zval* options_p TSRMLS_DC)
{
    as_policy_write             write_policy;
    uint32_t                    serializer_policy = -1;
    as_static_pool              static_pool = {0};
    as_record                   record;
    int16_t                     init_record = 0;
    uint16_t                    gen_value = 0;

    if ((!record_pp) || (!as_key_p) || (!error_p) || (!as_object_p)) {
        DEBUG_PHP_EXT_DEBUG("Unable to put record");
        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR, "Unable to put record");
        goto exit;
    }

    as_record_inita(&record, zend_hash_num_elements(Z_ARRVAL_PP(record_pp)));
    init_record = 1;

    set_policy(NULL, &write_policy, NULL, NULL, NULL, NULL, NULL,
            &serializer_policy, options_p, error_p TSRMLS_CC);
    if (AEROSPIKE_OK != (error_p->code)) {
        DEBUG_PHP_EXT_DEBUG("Unable to set policy");
        goto exit;
    }

    get_generation_value(options_p, &gen_value, error_p TSRMLS_CC);
    if (AEROSPIKE_OK != (error_p->code)) {
        DEBUG_PHP_EXT_DEBUG("Unable to set generation value");
        goto exit;
    }

    aerospike_transform_iterate_records(record_pp, &record, &static_pool,
            serializer_policy, error_p TSRMLS_CC);
    if (AEROSPIKE_OK != (error_p->code)) {
        DEBUG_PHP_EXT_DEBUG("Unable to put record");
        goto exit;
    }

    record.gen = gen_value;
    record.ttl = ttl_u32;
    aerospike_key_put(as_object_p, error_p, &write_policy, as_key_p, &record);

exit:
    /* clean up the as_* objects that were initialised */
    aerospike_helper_free_static_pool(&static_pool);

    /*policy_write, should it be destroyed ??? */
    if (init_record) {
        as_record_destroy(&record);
    }

    return error_p->code;
}

/* 
 *******************************************************************************************************
 * Read specified filter bins for the record specified by get_rec_key_p.
 *
 * @param as_object_p               The C client's aerospike object.
 * @param bins_array_p              The hashtable pointing to the PHP filter bins array.
 * @param get_record_p              The C client's as_record to be read into.
 * @param error_p                   The C client's as_error object to be
 *                                  populated by this functon.
 * @param get_rec_key_p             The C client's as_key that identifies the
 *                                  record to be read.
 * @param read_policy_p             The C client's as_policy_read to be used
 *                                  while reading the record.
 *
 * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_x.
 *******************************************************************************************************
 */
extern as_status
aerospike_transform_filter_bins_exists(aerospike *as_object_p,
                                       HashTable *bins_array_p,
                                       as_record **get_record_p,
                                       as_error *error_p,
                                       as_key *get_rec_key_p,
                                       as_policy_read *read_policy_p)
{
    int                 bins_count = zend_hash_num_elements(bins_array_p);
    as_status           status = AEROSPIKE_OK;
    uint                sel_cnt = 0;
    const char          *select[bins_count];
    HashPosition        pointer;
    zval                **bin_names;
    
    foreach_hashtable (bins_array_p, pointer, bin_names) {
        switch (Z_TYPE_PP(bin_names)) {
            case IS_STRING:
                select[sel_cnt++] = Z_STRVAL_PP(bin_names);
                break;
            default:
                status = AEROSPIKE_ERR_PARAM;
                goto exit;
        }
    }
    
    select[bins_count] = NULL;
    if (AEROSPIKE_OK != (status =
                aerospike_key_select(as_object_p, error_p, read_policy_p,
                        get_rec_key_p, select, get_record_p))) {
        goto exit;
    }
exit:
    return status;
}

extern as_status
aerospike_get_key_digest(as_key *key_p, char *ns_p, char *set_p, zval *pk_p,
        char **digest_pp TSRMLS_DC)
{
    as_status           status = AEROSPIKE_OK;

    if (AEROSPIKE_OK != (status = aerospike_add_key_params(key_p, Z_TYPE_P(pk_p), ns_p, set_p, &pk_p, 0))) {
        DEBUG_PHP_EXT_ERROR("Failed to initialize as_key");
        goto exit;
    }
    
    if (as_key_digest(key_p) && key_p->digest.init) {
        *digest_pp = (char *) key_p->digest.value;
    } else {
        *digest_pp = NULL;
        DEBUG_PHP_EXT_ERROR("Failed to compute digest");
        status = AEROSPIKE_ERR_CLIENT;
    }
    
exit:
    return status;
}

/*
 *******************************************************************************************************
 * Function that returns no. of digits of a positive integer.
 *******************************************************************************************************
 */
static int
numPlaces (int num) {
    int places = 1;
    if (num < 0) {
        return -1;
    }
    while (num > 9) {
        num /= 10;
        places++;
    }
    return places;
}

/* 
 *******************************************************************************************************
 * Creates php key(ns, set, pk) 
 *
 * @param ns_p                      Namespace
 * @param ns_p_length               Namespace length
 * @param set_p                     Set
 * @param set_p_length              Set length
 * @param pk_p                      Primary key of a record
 * @param is_digest                 The flag which indicates whether pk_p
 *                                  is primary key or digest.
 * @param reurn_value               Result of initkey
 * @param record_key_p              Key of a record
 * @param options_p                 The options array
 * @param get_flag                  The flag which indicates whether this function
 *                                  was called from get() API or other APIS.
 *
 * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_x.
 *******************************************************************************************************
 */
extern as_status
aerospike_init_php_key(char *ns_p, long ns_p_length, char *set_p,
        long set_p_length, zval *pk_p, bool is_digest, zval *return_value,
        as_key *record_key_p, zval *options_p, bool get_flag TSRMLS_DC)
{
    as_status       status = AEROSPIKE_OK;

    if (!ns_p || !set_p || !return_value) {
        DEBUG_PHP_EXT_DEBUG("Parameter error");
        status = AEROSPIKE_ERR;
        goto exit;
    }

    if (0 != add_assoc_stringl(return_value, PHP_AS_KEY_DEFINE_FOR_NS, ns_p, ns_p_length, 1)) {
        DEBUG_PHP_EXT_DEBUG("Unable to get namespace");
        status = AEROSPIKE_ERR;
        goto exit;
    }
    if ( 0 != add_assoc_stringl(return_value, PHP_AS_KEY_DEFINE_FOR_SET, set_p, set_p_length, 1)) {
        DEBUG_PHP_EXT_DEBUG("Unable to get set");
        status = AEROSPIKE_ERR;
        goto exit;
    }
    if (pk_p) {
        /*
         * pk_p != NULL in case of initKey().
         */
        switch(Z_TYPE_P(pk_p)) {
            case IS_LONG:
                if (!is_digest) {
                    add_assoc_long(return_value, PHP_AS_KEY_DEFINE_FOR_KEY, Z_LVAL_P(pk_p));
                } else {
                    if (numPlaces(Z_LVAL_P(pk_p)) > AS_DIGEST_VALUE_SIZE) {
                        DEBUG_PHP_EXT_ERROR("Aerospike::initKey() digest max length exceeded");
                        status = AEROSPIKE_ERR;
                        goto exit;
                    } 
                    add_assoc_long(return_value, PHP_AS_KEY_DEFINE_FOR_DIGEST, Z_LVAL_P(pk_p));
                }
                break;
            case IS_STRING:
                if (strlen(Z_STRVAL_P(pk_p)) == 0) {
                    DEBUG_PHP_EXT_ERROR("Aerospike::initKey() expects parameter 1-3 to be non-empty strings");
                    status = AEROSPIKE_ERR;
                    goto exit;
                }
                if (!is_digest) {
                    add_assoc_string(return_value, PHP_AS_KEY_DEFINE_FOR_KEY, Z_STRVAL_P(pk_p), 1);
                } else {
                    if (strlen(Z_STRVAL_P(pk_p)) > AS_DIGEST_VALUE_SIZE) {
                        DEBUG_PHP_EXT_ERROR("Aerospike::initKey() digest max length exceeded");
                        status = AEROSPIKE_ERR;
                        goto exit;
                    }
                    add_assoc_string(return_value, PHP_AS_KEY_DEFINE_FOR_DIGEST, Z_STRVAL_P(pk_p), 1);
                }
                break;
            default:
                DEBUG_PHP_EXT_ERROR("Aerospike::initKey() expects parameter 1-3 to be non-empty strings");
                status = AEROSPIKE_ERR;
                goto exit;
        }
    } else {
        /*
         * pk_p == NULL in case of get() or scan().
         */

        zval **key_policy_pp = NULL;

        if (!record_key_p) {
            status = AEROSPIKE_ERR;
            goto exit;
        }

        if (options_p) {
            /*
             * Optionally NOT NULL in case of get().
             * Always NULL in case of scan().
             */
            zend_hash_index_find(Z_ARRVAL_P(options_p), OPT_POLICY_KEY, (void **) &key_policy_pp);
        }

        if ((!record_key_p->valuep) || (get_flag && ((!key_policy_pp) || (key_policy_pp &&
                    Z_LVAL_PP(key_policy_pp) == AS_POLICY_KEY_DIGEST)))) {
            if (0 != add_assoc_null(return_value, PHP_AS_KEY_DEFINE_FOR_KEY)) {
                DEBUG_PHP_EXT_DEBUG("Unable to get primary key of a record");
                status = AEROSPIKE_ERR;
                goto exit;
            } else {
                goto exit;
            }
        }

        switch (((as_val*)(record_key_p->valuep))->type) {
            case AS_STRING:
                if (0 != add_assoc_string(return_value, PHP_AS_KEY_DEFINE_FOR_KEY, record_key_p->value.string.value, 1)) {
                    DEBUG_PHP_EXT_DEBUG("Unable to get primary of a record");
                    status = AEROSPIKE_ERR;
                    goto exit;
                }
                break;
            case AS_INTEGER:
                if (0 != add_assoc_long(return_value, PHP_AS_KEY_DEFINE_FOR_KEY, record_key_p->value.integer.value)) {
                    DEBUG_PHP_EXT_DEBUG("Unable to get primary of a record");
                    status = AEROSPIKE_ERR;
                    goto exit;
                }
                break;
            default:
                break;
        }
    }

exit:
    return status;
}

static char hexconvtab[] = "0123456789abcdef";

static char* bin2hex(const unsigned char *old, const int oldlen)
{
    unsigned char *result = NULL;
    size_t i, j;

    if (NULL != (result = (unsigned char *) malloc((oldlen * 2 * sizeof(char) + 1)))) {
        for (i = j = 0; i < oldlen; i++) {
            result[j++] = hexconvtab[old[i] >> 4];
            result[j++] = hexconvtab[old[i] & 15];
        }
        result[j] = '\0';
    }

    return (char *)result;
}

/* 
 *******************************************************************************************************
 * Get record key and key digest. 
 *
 * @param get_record_p              Record
 * @param record_key_p              Key of record
 * @param key_container_p           Key container
 * @param options                   Optional parameters
 * @param get_flag                  The flag which indicates whether this function
 *                                  was called from get() API or other APIS.
 *
 * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_x.
 *******************************************************************************************************
 */
static as_status
aerospike_get_record_key_digest(as_record* get_record_p, as_key *record_key_p, zval* key_container_p, zval* options_p, bool get_flag TSRMLS_DC)
{
    as_status                  status = AEROSPIKE_OK;
    php_unserialize_data_t     var_hash;
    //int8_t*                    hex_str_p = NULL;

    if (!get_record_p || !record_key_p || !key_container_p) {
        status = AEROSPIKE_ERR;
        goto exit;
    }

    if (AEROSPIKE_OK != (status = aerospike_init_php_key(record_key_p->ns, strlen(record_key_p->ns),
            record_key_p->set, strlen(record_key_p->set), NULL, false,
            key_container_p, record_key_p, options_p, get_flag TSRMLS_CC))) {
        DEBUG_PHP_EXT_DEBUG("Unable to get key of a record");
        status = AEROSPIKE_ERR;
        goto exit;
    }

    /* 
     * Returns allocated memory, need to free
     */
    /*if (NULL == (hex_str_p = bin2hex(((as_key_digest(record_key_p))->value), AS_DIGEST_VALUE_SIZE))) {
        DEBUG_PHP_EXT_DEBUG("Unable to allocate memory for digest");
        status = AEROSPIKE_ERR;
        goto exit;
    }*/

    if (record_key_p->digest.init && (0 != add_assoc_stringl(key_container_p,
                    PHP_AS_KEY_DEFINE_FOR_DIGEST,
                    (char *) record_key_p->digest.value,
                    AS_DIGEST_VALUE_SIZE, 1))) {
        DEBUG_PHP_EXT_DEBUG("Unable to get digest of a key");
        status = AEROSPIKE_ERR;
        goto exit;
    }

exit:
    /*if (hex_str_p) {
        free(hex_str_p);
    }*/

    return status;
}

/* 
 *******************************************************************************************************
 * Get record metadata(ttl, generation)
 *
 * @param get_record_p              Record
 * @param metadata_container_p      It holds metadata of a record.
 *
 * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_x.
 *******************************************************************************************************
 */
as_status
aerospike_get_record_metadata(as_record* get_record_p, zval* metadata_container_p TSRMLS_DC)
{
    as_status           status = AEROSPIKE_OK;

    if (!get_record_p) {
        DEBUG_PHP_EXT_DEBUG("Incorrect Record");
        status = AEROSPIKE_ERR;
        goto exit;
    }
    if (0 != add_assoc_long(metadata_container_p, PHP_AS_RECORD_DEFINE_FOR_TTL, get_record_p->ttl)) {
        DEBUG_PHP_EXT_DEBUG("Unable to get time to live of a record");
        status = AEROSPIKE_ERR;
        goto exit;

    }
    if (0 != add_assoc_long(metadata_container_p, PHP_AS_RECORD_DEFINE_FOR_GENERATION, get_record_p->gen)) {
        DEBUG_PHP_EXT_DEBUG("Unable to get generation of a record");
        status = AEROSPIKE_ERR;
        goto exit;
    }

exit:
    return status;
}

/* 
 *******************************************************************************************************
 * Get record key, metadata and bins of a record.
 *
 * @param get_record_p              Record
 * @param record_key_p              Key of a record
 * @param outer_container_p         Return value
 * @param options                   Optional parameters
 * @param get_flag                  The flag which indicates whether this function
 *                                  was called from get() API or other APIS.
 *
 * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_x.
 *******************************************************************************************************
 */
extern as_status
aerospike_get_key_meta_bins_of_record(as_record* get_record_p, as_key* record_key_p, zval* outer_container_p, zval* options_p, bool get_flag TSRMLS_DC)
{
    as_status           status = AEROSPIKE_OK;
    zval*               metadata_container_p = NULL;
    zval*               key_container_p = NULL;
    zval*               bins_container_p = NULL;

    if (!get_record_p || !record_key_p || ! outer_container_p) {
        DEBUG_PHP_EXT_DEBUG("Unable to get a record");
        status = AEROSPIKE_ERR;
        goto exit; 
    }

    MAKE_STD_ZVAL(key_container_p);
    array_init(key_container_p);
    status = aerospike_get_record_key_digest(get_record_p, record_key_p, key_container_p, options_p, get_flag TSRMLS_CC);
    if (status != AEROSPIKE_OK) {
        DEBUG_PHP_EXT_DEBUG("Unable to get key and digest for record");
        goto exit;
    }

    MAKE_STD_ZVAL(metadata_container_p);
    array_init(metadata_container_p);
    status = aerospike_get_record_metadata(get_record_p, metadata_container_p TSRMLS_CC);
    if (status != AEROSPIKE_OK) {
        DEBUG_PHP_EXT_DEBUG("Unable to get metadata of record");
        goto exit;
    }

    if (0 != add_assoc_zval(outer_container_p, PHP_AS_KEY_DEFINE_FOR_KEY, key_container_p)) {
        DEBUG_PHP_EXT_DEBUG("Unable to get key of a record");
        status = AEROSPIKE_ERR;
        goto exit;
    }
    if (0 != add_assoc_zval(outer_container_p, PHP_AS_RECORD_DEFINE_FOR_METADATA, metadata_container_p)) {
        DEBUG_PHP_EXT_DEBUG("Unable to get metadata of a record");
        status = AEROSPIKE_ERR;
        goto exit;
    }
exit:
    if (AEROSPIKE_OK != status) {
        if (key_container_p) {
            zval_ptr_dtor(&key_container_p);
        }

        if (metadata_container_p) {
            zval_ptr_dtor(&metadata_container_p);
        }
    }

    return status;
}

/* 
 *******************************************************************************************************
 * Read all bins for the record specified by get_rec_key_p.
 *
 * @param as_object_p               The C client's aerospike object.
 * @param get_rec_key_p             The C client's as_key that identifies the
 *                                  record to be read.
 * @param options_p                 The optional PHP parameters for Aerospike::get().
 * @param error_p                   The C client's as_error object to be
 *                                  populated by this functon.
 * @param get_record_p              The C client's as_record to be read into.
 * @param bins_p                    The optional PHP array for filter bins.
 *
 * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_x.
 *******************************************************************************************************
 */
extern as_status
aerospike_transform_get_record(Aerospike_object* aerospike_obj_p,
                               as_key* get_rec_key_p,
                               zval* options_p,
                               as_error *error_p,
                               zval* outer_container_p,
                               zval* bins_p TSRMLS_DC)
{
    as_status               status = AEROSPIKE_OK;
    as_policy_read          read_policy;
    as_record               *get_record = NULL;
    aerospike               *as_object_p = aerospike_obj_p->as_ref_p->as_p;
    foreach_callback_udata  foreach_record_callback_udata;
    zval*                   get_record_p = NULL;

    ALLOC_INIT_ZVAL(get_record_p);
    array_init(get_record_p);

    foreach_record_callback_udata.udata_p = get_record_p;
    foreach_record_callback_udata.error_p = error_p;
    foreach_record_callback_udata.obj = aerospike_obj_p;

    if ((!as_object_p) || (!get_rec_key_p) || (!error_p) || (!outer_container_p)) {
        status = AEROSPIKE_ERR;
        goto exit;
    }

    set_policy(&read_policy, NULL, NULL, NULL, NULL, NULL, NULL,
            NULL, options_p, error_p TSRMLS_CC);
    if (AEROSPIKE_OK != (status = (error_p->code))) {
        DEBUG_PHP_EXT_DEBUG("Unable to set policy");
        goto exit;
    }

    if (bins_p != NULL) {
        if (AEROSPIKE_OK != (status =
                    aerospike_transform_filter_bins_exists(as_object_p,
                            Z_ARRVAL_P(bins_p), &get_record, error_p,
                                    get_rec_key_p, &read_policy))) {
            goto exit;
        }
    } else if (AEROSPIKE_OK != (status = aerospike_key_get(as_object_p, error_p,
                    &read_policy, get_rec_key_p, &get_record))) {
        goto exit;
    }
    if (!as_record_foreach(get_record, (as_rec_foreach_callback) AS_DEFAULT_GET,
                &foreach_record_callback_udata)) {
        status = AEROSPIKE_ERR_SERVER;
        goto exit;
    }

    if (AEROSPIKE_OK != (status = aerospike_get_key_meta_bins_of_record(get_record, get_rec_key_p, outer_container_p, options_p, true TSRMLS_CC))) {
        DEBUG_PHP_EXT_DEBUG("Unable to get record key and metadata");
        status = AEROSPIKE_ERR;
        goto exit;
    }

    if (0 != add_assoc_zval(outer_container_p, PHP_AS_RECORD_DEFINE_FOR_BINS, get_record_p))    {
        DEBUG_PHP_EXT_DEBUG("Unable to get a record");
        status = AEROSPIKE_ERR;
        goto exit;
    }

exit:
    if (get_record) {
        as_record_destroy(get_record);
    }

    if (AEROSPIKE_OK != status) {
        if (get_record_p) {
            zval_ptr_dtor(&get_record_p);
        }
    }

    return status;
}
