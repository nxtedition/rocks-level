#!/bin/bash
set -e

echo "Building image..."
docker build --iidfile prebuilds.iid .

echo "Extracting prebuilds from image..."
IMG=$(cat prebuilds.iid)
ID=$(docker create $IMG)
docker cp "$ID:/rocks-level/prebuilds" ./

echo "Cleaning up..."
docker rm $ID > /dev/null
rm prebuilds.iid

echo "All done!"
