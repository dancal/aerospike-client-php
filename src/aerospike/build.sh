#!/bin/bash
################################################################################
# Copyright 2013-2015 Aerospike, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
################################################################################

export CLIENTREPO_FLATFORM=`/bin/uname -s`
export CLIENTREPO_SYSTEM=`/bin/uname -m`
export CLIENTREPO_3X=${PWD}/../../../aerospike-client-c
export CLIENTREPO_INC=${PWD}/../../../aerospike-client-c/target/${CLIENTREPO_FLATFORM}-${CLIENTREPO_SYSTEM}/include
<<<<<<< HEAD
export AEROSPIKE_C_CLIENT=${AEROSPIKE_C_CLIENT:-3.1.8}
if [[ ! -d $CLIENTREPO_3X || ! `ls $CLIENTREPO_3X/package/aerospike-client-c-devel-${AEROSPIKE_C_CLIENT}* 2> /dev/null` ]]; then
    rm -rf $CLIENTREPO_3X/package
=======
export AEROSPIKE_C_CLIENT=${AEROSPIKE_C_CLIENT:-3.0.94}
if [ ! -d $CLIENTREPO_3X ]; then
>>>>>>> b7d410704154747d511561cced2b3f04da742d56
    echo "Downloading Aerospike C Client SDK $AEROSPIKE_C_CLIENT"
else
    echo "Aerospike C Client SDK is present."
fi
<<<<<<< HEAD
#$CLIENTREPO_3X/scripts/aerospike-client-c.sh
=======
#scripts/aerospike-client-c.sh
>>>>>>> b7d410704154747d511561cced2b3f04da742d56
#if [ $? -gt 0 ]; then
#    exit 1
#fi

LOGLEVEL="PHP_EXT_AS_LOG_LEVEL_OFF"

parse_args () {
    while [[ $# > 1 ]]
    do
        key="$1"
        shift

        case $key in
            -l|--loglevel)
                case "$1" in
                    TRACE|trace)
                        LOGLEVEL="AS_LOG_LEVEL_TRACE"
                        ;;
                    DEBUG|debug)
                        LOGLEVEL="AS_LOG_LEVEL_DEBUG"
                        ;;
                    INFO|info)
                        LOGLEVEL="AS_LOG_LEVEL_INFO"
                        ;;
                    WARN|warn)
                        LOGLEVEL="AS_LOG_LEVEL_WARN"
                        ;;
                    ERROR|error)
                        LOGLEVEL="AS_LOG_LEVEL_ERROR"
                        ;;
                    OFF|off)
                        LOGLEVEL="PHP_EXT_AS_LOG_LEVEL_OFF"
                        ;;
                    *)
                        echo "Usage:"
                        echo "build -l|--loglevel <ONE AMONG: TRACE, DEBUG, INFO, WARN, ERROR, OFF> [-h|--help]"
                        exit 1
                        ;;
                esac
                shift
                ;;
            -h|--help)
                echo "Usage:"
                echo "build -l|--loglevel <ONE AMONG: TRACE, DEBUG, INFO, WARN, ERROR, OFF> [-h|--help]"
                shift
                ;;
            --default)
                echo "Using default Loglevel."
                LOGLEVEL="PHP_EXT_AS_LOG_LEVEL_OFF"
                shift
                ;;
            *)
                echo "Usage:"
                echo "build -l|--loglevel <ONE AMONG: TRACE, DEBUG, INFO, WARN, ERROR, OFF> [-h|--help]"
                exit 1
                ;;
        esac
    done
}

<<<<<<< HEAD
if [ ! -z "$(which php-config)" ]; then
    PHP_CONFIG=`which php-config`
else
    PHP_CONFIG="php-config"
fi
=======
#if [ ! -z "$(which php-config)" ]; then
#    PHP_CONFIG=`which php-config`
#else
#    PHP_CONFIG="php-config"
#fi

>>>>>>> b7d410704154747d511561cced2b3f04da742d56
PHP_CONFIG="/usr/bin/php-config"

if [ -f Makefile ]; then
  make clean
fi
parse_args $@
/usr/bin/phpize
./configure --enable-aerospike --with-php-config=$PHP_CONFIG "CFLAGS=-g -O3"

OS=`uname`
INCLUDE_LUA_5_1=/usr/include/lua5.1
if [ -d $INCLUDE_LUA_5_1 ] ; then
    LUA_SUFFIX=5.1
fi

#CFLAGS="-g -D__AEROSPIKE_PHP_CLIENT_LOG_LEVEL__=${LOGLEVEL}"
CFLAGS="-g -D__AEROSPIKE_PHP_CLIENT_LOG_LEVEL__=${LOGLEVEL} -I${CLIENTREPO_INC}/ck -I${CLIENTREPO_INC}"

if [ $OS = "Darwin" ] ; then
    ec=`ls /usr/local/lib/liblua.a`
    if [ $? -eq 0 ] ; then
        LIBLUA="-L/usr/local/lib -llua"
        ec=`ls /usr/local/include/lua.h || ls /usr/local/include/lualib.h`
        if [ $? -eq 0 ] ; then
            INCLUDE_LUA_5_1=/usr/local/include
        fi
    else
        LIBLUA="-llua"
    fi
    LDFLAGS="-L$CLIENTREPO_3X/lib -laerospike -lcrypto $LIBLUA"
else
    LDFLAGS="-Wl,-Bstatic -L$CLIENTREPO_3X/lib -laerospike -Wl,-Bdynamic -llua$LUA_SUFFIX"
    # Find and link to libcrypto (provided by OpenSSL)
    ec=`ls /usr/lib64/libcrypto.a || ls /usr/lib/libcrypto.so`
    if [ $? -eq 0 ] ; then
        LIBCRYPTO="-L/usr/local/lib -lcrypto"
    else
        ec=`ls /usr/local/ssl/lib/libcrypto.a || ls /usr/local/ssl/lib/libcrypto.so`
        if [ $? -eq 0 ] ; then
            LIBCRYPTO="-L/usr/local/ssl/lib -lcrypto"
        else
            ec=`ls /usr/lib64/libcrypto.a || ls /usr/lib64/libcrypto.so`
            if [ $? -eq 0 ] ; then
                LIBCRYPTO="-L/usr/lib64 -lcrypto"
            else
                ec=`ls /usr/lib/x86_64-linux-gnu/libcrypto.a || ls /usr/lib/x86_64-linux-gnu/libcrypto.so`
                if [ $? -eq 0 ] ; then
                    LIBCRYPTO="-L/usr/lib/x86_64-linux-gnu -lcrypto"
                else
                    ec=`ls /usr/lib/libcrypto.a || ls /usr/lib/libcrypto.so`
                    if [ $? -eq 0 ] ; then
                        LIBCRYPTO="-L/usr/lib -lcrypto"
                    fi
                fi
            fi
        fi
    fi
    LDFLAGS="$LDFLAGS $LIBCRYPTO -lrt"
fi

make clean all "CFLAGS=$CFLAGS" "EXTRA_INCLUDES+=-I${CLIENTREPO_INC}/ck -I${CLIENTREPO_INC} -I$INCLUDE_LUA_5_1" "EXTRA_LDFLAGS=$LDFLAGS"
if [ $? -gt 0 ] ; then
    echo "The build has failed...exiting"
    exit 2
fi
scripts/test-cleanup.sh

headline()
{
    printf "\n⇒  ⇒  ⇒  $1  ⇐  ⇐  ⇐\n"
}

seperator()
{
    echo ""
}

code()
{
    echo "    $1"
}

config()
{
    headline "Configure PHP to Load the Extension"
    INI_PATH=`echo $2|cut -d '>' -f2`
    echo "$1 file at $INI_PATH with the directive:"
    code "extension=aerospike.so"
    if [ -f /opt/aerospike/client-php/sys-lua/aerospike.lua ]; then
        code "aerospike.udf.lua_system_path=/opt/aerospike/client-php/sys-lua"
        if [ -d /opt/aerospike/client-php/usr-lua ]; then
            code "aerospike.udf.lua_user_path=/opt/aerospike/client-php/usr-lua"
            if [ ! -f /opt/aerospike/client-php/usr-lua/test_transform.lua ]; then
                cp ./tests/lua/*.lua /opt/aerospike/client-php/usr-lua/
                if [ $? -gt 0 ] ; then
                    echo "Failed to copy the Lua user files.  Please run:"
                    code "sudo cp tests/lua/*.lua /opt/aerospike/client-php/usr-lua/"
                fi
            fi
        fi
    elif [ -f /usr/local/aerospike/client-php/sys-lua/aerospike.lua ]; then
        code "aerospike.udf.lua_system_path=/usr/local/aerospike/client-php/sys-lua"
        if [ -d /usr/local/aerospike/client-php/usr-lua ]; then
            code "aerospike.udf.lua_user_path=/usr/local/aerospike/client-php/usr-lua"
            if [ ! -f /usr/local/aerospike/client-php/usr-lua/test_transform.lua ]; then
                cp ./tests/lua/*.lua /usr/local/aerospike/client-php/usr-lua/
                if [ $? -gt 0 ] ; then
                    echo "Failed to copy the Lua user files.  Please run:"
                    code "sudo cp tests/lua/*.lua /usr/local/aerospike/client-php/usr-lua/"
                fi
            fi
        fi
    fi
    headline "Verify the Extension"
    code "php -m | grep aerospike"
    seperator

	make install
}

echo "----------------------------------------------------------------------"
headline "Installing the Aerospike PHP Extension"
echo "To install the extension run inside src/aerospike:"
code "sudo make install"

HAS_PHP_CONF_D=`php -i 2>&1 | grep "Scan this dir for additional .ini files"`
if [ $? -eq 0 ] ; then
    config "Create an aerospike.ini" "$HAS_PHP_CONF_D"
    exit 0
fi

HAS_PHP_INI=`php -i 2>&1 | grep "Loaded Configuration File"`
if [ $? -eq 0 ] ; then
    config "Edit the php.ini" "$HAS_PHP_INI"
fi
