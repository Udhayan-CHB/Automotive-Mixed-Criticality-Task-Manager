#!/bin/sh
set -eu

for project in \
    VehicleIO \
    ResourceMonitor \
    SafetyMonitor \
    SafetyTask \
    ControlTask \
    Infotainment \
    FaultInjector \
    TraceViewer
do
    echo "[BUILD] $project"
    make -C "$project" clean all
done
