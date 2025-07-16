#!/bin/bash

declare -A containers
containers=(
  [rfsim5g-oai-nr-ue1]=5201
  [rfsim5g-oai-nr-ue2]=5202
  [rfsim5g-oai-nr-ue3]=5203
  [rfsim5g-oai-nr-ue4]=5204
  [rfsim5g-oai-nr-ue5]=5205
  [rfsim5g-oai-nr-ue6]=5206
  [rfsim5g-oai-nr-ue7]=5207
  [rfsim5g-oai-nr-ue8]=5208
)

for name in "${!containers[@]}"; do
  port=${containers[$name]}
  echo "Launching iperf3 in container $name on port $port..."
  docker exec -d "$name" bash -c "ip route add 10.0.1.0/24 dev oaitun_ue1 && iperf3 -c 10.0.1.1 -p $port -t 1000"
done
