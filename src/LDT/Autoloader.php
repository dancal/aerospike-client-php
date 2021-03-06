<?php
/**
 * Copyright 2014-2015 Aerospike, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * @category   Database
 * @author     Ronen Botzer <rbotzer@aerospike.com>
 * @copyright  Copyright 2013-2014 Aerospike, Inc.
 * @license    http://www.apache.org/licenses/LICENSE-2.0 Apache License, Version 2
 * @filesource
 */
namespace Aerospike\LDT;

/**
 * Autoloader class for classes in the \Aerospike\LDT namespace.
 * In the event the LDT library is not being loaded by Composer
 * Autoloader::register() it with spl.
 *
 */
class Autoloader
{
    /**
     * Call to register the Aerospike\LDT\Autoloader::load() method
     */
    public static function register()
    {
        spl_autoload_register(array(__CLASS__, 'load'));
    }

    /**
     * The loading function for a $class_name in the \Aerospike\LDT namespace
     *
     * @param string $class_name
     */
    public static function load($class_name)
    {
        $parts = explode('Aerospike\\LDT\\', $class_name);
        require __DIR__. DIRECTORY_SEPARATOR. $parts[1]. '.php';
    }
}

?>
