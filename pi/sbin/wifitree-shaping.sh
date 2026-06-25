#!/bin/bash
set -e
export PATH=$PATH:/usr/sbin:/sbin
AP=uap0
IFB=ifb0
BW_DOWN=20mbit
BW_UP=20mbit

modprobe ifb numifbs=1 2>/dev/null || true
modprobe sch_cake 2>/dev/null || true

ip link add "$IFB" type ifb 2>/dev/null || true
ip link set "$IFB" up

tc qdisc del dev "$AP" ingress 2>/dev/null || true
tc qdisc add dev "$AP" handle ffff: ingress
tc filter add dev "$AP" parent ffff: matchall action mirred egress redirect dev "$IFB"

tc qdisc del dev "$IFB" root 2>/dev/null || true
tc qdisc add dev "$IFB" root cake bandwidth "$BW_UP" triple-isolate nat

tc qdisc del dev "$AP" root 2>/dev/null || true
tc qdisc add dev "$AP" root cake bandwidth "$BW_DOWN" triple-isolate nat
