#!/usr/bin/env bash
ROOT_DIR=$( (cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P) )

shopt -s expand_aliases
alias error='_error "${LINENO:-0}" "${FUNCNAME[*]:-main}"'
function _error() {
  local return_code=$?
  local lineno=${1:-0}
  local funcname=${2:-main}
  shift 2
  local message=${*:-"no message"}
  message="Error: ${message}"
  local message_rc
  [[ $return_code != 0 ]] && message_rc="exited with return code ${return_code}.\n"
  printf "%s:%d (%s): ${message_rc}%s\n" "${BASH_SOURCE[0]}" "${lineno}" "${funcname}" "${message}"
  exit 1
}

(
  cd "$ROOT_DIR/td" || error "cd failed"
  cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX:PATH="$ROOT_DIR/td/install" || error "cmake configure failed"
  cmake --build build --target install || error "cmake build failed"
) || error "tdlib build failed"

(
  rm "$ROOT_DIR/telegram-bot-api/td"
  ln -s "$ROOT_DIR/td" "$ROOT_DIR/telegram-bot-api/td"

  cd "$ROOT_DIR/telegram-bot-api" || error "cd failed"
  cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX:PATH="$ROOT_DIR/telegram-bot-api/install" || error "cmake configure failed"
  cmake --build build --target install || error "cmake build failed"
) || error "tdlib post build failed"

(
  cd "$ROOT_DIR/libtuntap" || error "cd failed"
#  export CC=clang
#  export CXX=clang++
#  cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DENABLE_CXX=ON -DENABLE_PYTHON=ON -DCMAKE_INSTALL_PREFIX:PATH="$ROOT_DIR/libtuntap/install" || error "cmake configure failed"
  cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_CXX=ON -DENABLE_PYTHON=ON -DCMAKE_INSTALL_PREFIX:PATH="$ROOT_DIR/libtuntap/install" || error "cmake configure failed"
  cmake --build build --target install || error "cmake build failed"
) || error "tdlib post build failed"
