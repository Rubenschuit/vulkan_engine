#!/bin/bash

if (( $1 == 'clean' ))
then
  rm -rf build/
  rm -rf shaders/*.spv
  echo "Cleaned build directory"
  exit 0
fi

mkdir -p build
cd build
cmake -S ../ -B .
make && make Shaders && ./VEngine
cd ..