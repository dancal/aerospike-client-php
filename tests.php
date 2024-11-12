#!/usr/bin/env php
<?php

function checkMemoryUsage($operation) {
    echo $operation . " - Memory Usage: " . memory_get_usage() . " bytes\n";
}

// Helper function for status check
function checkStatus($status, $operation) {
    echo $operation . ": " . ($status == Aerospike::OK ? "OK" : "FAIL") . "\n";
}


//for($i=0;$i<100;$i++) {
checkMemoryUsage("Initial");

// Aerospike 서버 연결 설정
$config = ["hosts" => [["addr" => "192.168.0.150", "port" => 3000]]];
$aerospike = new Aerospike($config, false);
if (!$aerospike->isConnected()) {
    die("Failed to connect to Aerospike server\n");
}

echo "Connected to Aerospike\n\n";

// 테스트용 기본 키 생성
$key = $aerospike->initKey("memory", "users", "user123");
$list_key = $aerospike->initKey("memory", "users", "user_list");

// 기본 데이터 삽입
$aerospike->put($key, ["name" => "John Doe", "age" => 30]);
$aerospike->put($list_key, ["items" => ["apple"]]);

// 여러 키로 getMany 호출
echo "\nTesting getMany API:\n";
$keys = [
    $aerospike->initKey("memory", "users", "user123"),
    $aerospike->initKey("memory", "users", "user_list"),
];
$status = $aerospike->getMany($keys, $records);
checkStatus($status, "getMany");
if ($status == Aerospike::OK) {
    echo "Records from getMany:\n";
    //print_r($records);
}

// -------------------
// List 관련 메서드
// -------------------
checkStatus($aerospike->listClear($list_key, "items"), "List Clear");

checkStatus($aerospike->listAppend($list_key, "items", "banana"), "List Append");

checkStatus($aerospike->listInsert($list_key, "items", 0, "mango"), "List Insert");

checkStatus($aerospike->listInsertItems($list_key, "items", 1, ["kiwi", "grape"]), "List Insert Items");

$status = $aerospike->listGet($list_key, "items", 1, $item);
checkStatus($status, "List Get");
if ($status == Aerospike::OK) print_r($item);

$status = $aerospike->listGetRange($list_key, "items", 0, 3, $range);
checkStatus($status, "List Get Range");
if ($status == Aerospike::OK) print_r($range);

// 리스트에서 항목 제거 후 결과 저장 (listPop)
$status = $aerospike->listPop($list_key, "items", 0, $popped_item);
checkStatus($status, "List Pop");
if ($status == Aerospike::OK) {
    //echo "Popped Item: ";
    //print_r($popped_item);
}

$status = $aerospike->listPopRange($list_key, "items", 0, 2, $range);
checkStatus($status, "List Pop Range");
if ($status == Aerospike::OK) print_r($range);

checkStatus($aerospike->listRemove($list_key, "items", 1), "List Remove");

checkStatus($aerospike->listRemoveRange($list_key, "items", 0, 1), "List Remove Range");

$status = $aerospike->listSize($list_key, "items", $size);
checkStatus($status, "List Size");
if ($status == Aerospike::OK) echo "Size of list: " . $size . "\n";

checkStatus($aerospike->listSet($list_key, "items", 0, "orange"), "List Set");

checkStatus($aerospike->listTrim($list_key, "items", 0, 1), "List Trim");

// -------------------
// Predicate 관련 메서드
// -------------------
echo "Predicate API Tests:\n";
//print_r($aerospike->predicateEquals("age", 30));
// Predicate Range 테스트
//$status = $aerospike->predicateRange("age", 20, 40, Aerospike::INDEX_NUMERIC);
//checkStatus($status, "Predicate Range");
//print_r($aerospike->predicateContains("items", Aerospike::INDEX_TYPE_LIST, "banana"));
//print_r($aerospike->predicateBetween("age", 25, 35));

// -------------------
// Security 관련 메서드 (사용 권한 필요)
// -------------------
/*
echo "\nSecurity API Tests:\n";

// 새 사용자 및 역할 생성
checkStatus($aerospike->createRole("testRole", ["read", "write"], ["testNamespace"]), "Create Role");
checkStatus($aerospike->createUser("testUser", "testPassword", ["testRole"]), "Create User");

// 역할 및 권한 관리
checkStatus($aerospike->grantRoles("testUser", ["read", "write"]), "Grant Roles");
checkStatus($aerospike->revokeRoles("testUser", ["read"]), "Revoke Roles");

checkStatus($aerospike->grantPrivileges("testRole", [["code" => Aerospike::PRIV_USER_ADMIN]]), "Grant Privileges");
checkStatus($aerospike->revokePrivileges("testRole", [["code" => Aerospike::PRIV_USER_ADMIN]]), "Revoke Privileges");

// 사용자 및 역할 정보 조회
$status = $aerospike->queryRole("testRole", $role);
checkStatus($status, "Query Role");
if ($status == Aerospike::OK) print_r($role);

$status = $aerospike->queryRoles($roles);
checkStatus($status, "Query Roles");
if ($status == Aerospike::OK) print_r($roles);

$status = $aerospike->queryUser("testUser", $user);
checkStatus($status, "Query User");
if ($status == Aerospike::OK) print_r($user);

$status = $aerospike->queryUsers($users);
checkStatus($status, "Query Users");
if ($status == Aerospike::OK) print_r($users);

// 비밀번호 변경
checkStatus($aerospike->changePassword("testUser", "newPassword"), "Change Password");

// 사용자 및 역할 삭제
checkStatus($aerospike->dropRole("testRole"), "Drop Role");
checkStatus($aerospike->dropUser("testUser"), "Drop User");
 */

// 스캔 작업 수행
echo "Scanning 'memory' set:\n";
$status = $aerospike->scan("memory", "users", function ($record) {
    //print_r($record);
});
checkStatus($status, "Scan");

checkStatus($aerospike->setLogLevel(Aerospike::LOG_LEVEL_DEBUG), "Set Log Level");

// -------------------
// 데이터 및 연결 종료
// -------------------
$aerospike->remove($key);
$aerospike->remove($list_key);
$aerospike->close();
echo "Disconnected from Aerospike\n";

checkMemoryUsage("After Close");

//}
