#!/bin/bash

PKG_VERSION=$1
PKG_URL="${PACKAGE_REGISTRY_URL}/${PKG_VERSION}"


function upload_file() {
    path=$1
    filename=$2

    curl -sS -f --header "JOB-TOKEN: $CI_JOB_TOKEN" --upload-file "$path" "${PKG_URL}/${filename}"
}

upload_file "build/Release/server-utils.dll" "server-utils.dll"