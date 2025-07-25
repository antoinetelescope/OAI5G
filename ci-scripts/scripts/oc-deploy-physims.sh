#!/bin/bash

set -e

function die() { echo $@; exit 1; }
[ $# -eq 4 ] || die "usage: $0 <namespace> <release> <image tag> <oai directory>"

OC_NS=${1}
OC_RELEASE=${2}
IMG_TAG=${3}
OAI_DIR=${4}

cat /opt/oc-password | oc login -u oaicicd --server https://api.oai.cs.eurecom.fr:6443 > /dev/null
oc project ${OC_NS} > /dev/null
oc tag oaicicd-ran/oai-physim:${IMG_TAG} ${OC_NS}/oai-physim:${IMG_TAG}
helm install ${OC_RELEASE} ${OAI_DIR}/charts/${OC_RELEASE} --set global.image.version=${IMG_TAG} --wait
POD_ID=$(oc get pods | grep oai-${OC_RELEASE} | awk '{print $1}')
sleep 10
echo "Monitoring logs for 'FINISHED' in pod '$POD_ID'"
oc logs -f -n ${OC_NS} "$POD_ID" | while read -r line; do
  if [[ "$line" == *"FINISHED"* ]]; then
    echo "'FINISHED' detected in logs. Copying logs..."
    oc logs -n ${OC_NS} "$POD_ID" >> ${OAI_DIR}/ci-scripts/physim_log.txt
    oc describe pod $POD_ID >> ${OAI_DIR}/ci-scripts/physim_pods_summary.txt
    oc cp "$POD_ID":/opt/oai-physim/Testing/Temporary/LastTestsFailed.log ${OAI_DIR}/ci-scripts/LastTestsFailed.log
    oc cp "$POD_ID":/opt/oai-physim/${OC_RELEASE}-tests.json ${OAI_DIR}/ci-scripts/${OC_RELEASE}-tests.json
    oc cp "$POD_ID":/opt/oai-physim/${OC_RELEASE}-run.xml ${OAI_DIR}/ci-scripts/${OC_RELEASE}-run.xml
    break
  fi
done
helm uninstall ${OC_RELEASE} --wait
oc delete istag oai-physim:${IMG_TAG} -n ${OC_NS}
oc logout > /dev/null
