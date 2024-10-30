#!/bin/bash
################################################################################
# Aerospike Client C 설치 스크립트 (압축 해제 후 .deb 파일 자동 설치)
################################################################################

CWD=$(pwd)
SCRIPT_DIR=$(dirname $0)
BASE_DIR=$(cd "${SCRIPT_DIR}/.."; pwd)
AEROSPIKE=${CWD}/../aerospike-client-c
DOWNLOAD_DIR=${AEROSPIKE}/package
AEROSPIKE_C_VERSION=${AEROSPIKE_C_VERSION:-'6.4.4'}

DOWNLOAD=${DOWNLOAD_C_CLIENT:-1}
COPY_FILES=1
LIB_PATH=${PREFIX}

################################################################################
# FUNCTIONS
################################################################################

has_cmd() {
  hash "$1" 2> /dev/null
}

download_and_extract_tgz() {
  version=$1
  dest_dir=$2
  artifact="aerospike-client-c_${version}_debian12_x86_64.tgz"
  url="https://artifacts.aerospike.com/aerospike-client-c/${version}/${artifact}"
  dest="${dest_dir}/${artifact}"

  mkdir -p ${dest_dir}
  printf "info: downloading '%s' to '%s'\n" "${url}" "${dest}"

  # 다운로드
  if has_cmd curl; then
    curl -L ${url} -o ${dest}
  elif has_cmd wget; then
    wget -O ${dest} ${url}
  else
    echo "error: 'curl' 또는 'wget'이 필요합니다."
    exit 1
  fi

  # 디렉토리 없이 압축 해제
  printf "info: extracting '%s' without directories\n" "${dest}"
  tar --strip-components=1 -xzf "${dest}" -C ${AEROSPIKE}/package
  rm -f "${dest}"  # 압축 파일 삭제
}

install_deb_files() {
  # .deb 파일을 자동으로 설치
  for deb_file in ${DOWNLOAD_DIR}/*.deb; do
    if [ -f "$deb_file" ]; then
      printf "info: installing '%s'\n" "$deb_file"
      sudo dpkg -i "$deb_file"  # .deb 파일 설치
    fi
  done
}

################################################################################
# MAIN EXECUTION
################################################################################

if [ $DOWNLOAD -eq 1 ]; then
  download_and_extract_tgz ${AEROSPIKE_C_VERSION} ${DOWNLOAD_DIR}
  install_deb_files
fi

################################################################################
# LIBRARY PATH 및 필요한 파일 확인
################################################################################

LIB_PATH=/usr
AEROSPIKE_LIBRARY=${LIB_PATH}/lib/libaerospike.a
AEROSPIKE_INCLUDE=${LIB_PATH}/include

printf "\nCHECK\n"
if [ -f ${AEROSPIKE_LIBRARY} ]; then
  printf "   [✓] %s\n" "${AEROSPIKE_LIBRARY}"
else
  printf "   [✗] %s\n" "${AEROSPIKE_LIBRARY}"
  exit 1
fi

if [ -f ${AEROSPIKE_INCLUDE}/aerospike/aerospike.h ]; then
  printf "   [✓] %s\n" "${AEROSPIKE_INCLUDE}/aerospike/aerospike.h"
else
  printf "   [✗] %s\n" "${AEROSPIKE_INCLUDE}/aerospike/aerospike.h"
  exit 1
fi

################################################################################
# AEROSPIKE CLIENT 폴더에 파일 복사
################################################################################

if [ $COPY_FILES -eq 1 ]; then
  rm -rf ${AEROSPIKE}/{lib,include}
  mkdir -p ${AEROSPIKE}/{lib,include}
  cp ${AEROSPIKE_LIBRARY} ${AEROSPIKE}/lib/
  cp -R ${AEROSPIKE_INCLUDE}/{aerospike,citrusleaf} ${AEROSPIKE}/include/
fi

printf "Installation completed successfully.\n"

