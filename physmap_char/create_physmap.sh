#!/bin/bash

CONF=${1:-/etc/metax.conf}
SIZE=${2:-64G}
CMD=./physmapctl

while read -r host_id gpu_id local bdf bar rest || [[ -n "$host_id$gpu_id$local$bdf$bar$rest" ]]; do
    # 跳过空行、注释行、表头行
    [[ -z "$host_id" ]] && continue
    [[ "$host_id" =~ ^# ]] && continue
    [[ "$host_id" == "Topology" ]] && continue

    # 去掉前导 0，避免 08/09 被 bash 当成八进制
    h=$((10#$host_id))
    g=$((10#$gpu_id))

    name="h${h}-${g}"

    echo "$CMD create $name $bar $SIZE"
    $CMD create "$name" "$bar" "$SIZE"

done < "$CONF"
