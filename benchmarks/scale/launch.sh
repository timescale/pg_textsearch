#!/usr/bin/env bash
# Run from an authenticated controller; cloud identifiers stay in environment.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${AZURE_SUBSCRIPTION_ID:?Set the subscription explicitly}"
: "${AZURE_RESOURCE_GROUP:?Set a new dedicated searchbench-* group}"
: "${CONTROLLER_IPV4:?Set the controller public IPv4 address}"
: "${SSH_PRIVATE_KEY:?Set an existing benchmark SSH private key path}"
SSH_PUBLIC_KEY="${SSH_PUBLIC_KEY:-$SSH_PRIVATE_KEY.pub}"
STATE="${STATE:-$HERE/artifacts/azure.json}"
DATASET="${DATASET:-msmarco-v2}"
RUN_ID="${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
NATIVE_SOURCE="${PGTS_NATIVE_SOURCE:-8923a5ba16df2731627dfbde6f6e3b62e5b8273b}"
ACTION="${1:-all}"
case "$ACTION" in
    all|provision|setup|run|collect) ;;
    *) echo "Invalid action" >&2; exit 2 ;;
esac
case "$DATASET" in
    msmarco-v2|wikipedia|smoke) ;;
    *) echo "Invalid dataset" >&2; exit 2 ;;
esac
[[ "$RUN_ID" =~ ^[A-Za-z0-9_-]+$ ]] || { echo "Invalid run ID" >&2; exit 2; }
[[ "$NATIVE_SOURCE" =~ ^[0-9a-f]{40}$ ]] || {
    echo "PGTS_NATIVE_SOURCE must be 40 lowercase hexadecimal characters" >&2
    exit 2
}
ACCEPT="${MSMARCO_ACCEPT_TERMS:-0}"
case "$ACCEPT" in
    0|1) ;;
    *) echo "Use MSMARCO_ACCEPT_TERMS=0 or 1" >&2; exit 2 ;;
esac
if [[ ( "$ACTION" == all || "$ACTION" == run ) \
      && "$DATASET" == msmarco-v2 && "$ACCEPT" != 1 ]]; then
    echo "MS MARCO terms acceptance is required before allocating/running." >&2
    exit 2
fi
if [[ "$ACTION" == all || "$ACTION" == provision ]]; then
    RESUME_ARGS=()
    if [[ "${RESUME:-0}" == 1 ]]; then RESUME_ARGS+=(--resume); fi
    python3 "$HERE/azure.py" create \
        --subscription "$AZURE_SUBSCRIPTION_ID" \
        --group "$AZURE_RESOURCE_GROUP" --ssh-key "$SSH_PUBLIC_KEY" \
        --ssh-cidr "$CONTROLLER_IPV4/32" --state "$STATE" \
        "${RESUME_ARGS[@]}"
fi
if [[ "$ACTION" == provision ]]; then exit 0; fi
STATUS="$(python3 "$HERE/azure.py" status \
    --subscription "$AZURE_SUBSCRIPTION_ID" --group "$AZURE_RESOURCE_GROUP" \
    --state "$STATE")"
IP="$(printf '%s' "$STATUS" | python3 -c \
    'import ipaddress,json,sys; '\
'print(ipaddress.IPv4Address('\
'json.load(sys.stdin)["networkInterface"]["publicIpAddress"]))')"
COMPUTE_ID="$(printf '%s' "$STATUS" | python3 -c \
    'import json,sys; print(json.load(sys.stdin)["computeId"])')"
mkdir -p "$(dirname "$STATE")"
SSH=(ssh -i "$SSH_PRIVATE_KEY" -o BatchMode=yes -o ConnectTimeout=10 \
    -o "UserKnownHostsFile=$STATE.known_hosts" \
    -o StrictHostKeyChecking=accept-new)
REMOTE="bench@$IP"
READY=0
for attempt in {1..60}; do
    if "${SSH[@]}" "$REMOTE" true; then READY=1; break; fi
    sleep 10
done
[[ "$READY" == 1 ]] || { echo "SSH did not become ready" >&2; exit 1; }
printf -v RSYNC_SSH '%q ' "${SSH[@]}"
if [[ "$ACTION" == collect ]]; then
    RESULTS_DIR="${RESULTS_DIR:-$HERE/artifacts/runs}"
    mkdir -p "$RESULTS_DIR/$RUN_ID"
    rsync -az -e "$RSYNC_SSH" \
        "$REMOTE:/srv/searchbench/pg_textsearch-scale/runs/$RUN_ID/" \
        "$RESULTS_DIR/$RUN_ID/"
    exit 0
fi
if [[ "$ACTION" == all || "$ACTION" == setup ]]; then
    "${SSH[@]}" "$REMOTE" 'mkdir -p ~/bootstrap'
    scp -i "$SSH_PRIVATE_KEY" -o BatchMode=yes \
        -o "UserKnownHostsFile=$STATE.known_hosts" \
        "$HERE/bootstrap.sh" "$HERE/bootstrap_disks.py" \
        "$HERE/install_go.sh" "$REMOTE:bootstrap/"
    "${SSH[@]}" "$REMOTE" \
        "sudo -A bash ~/bootstrap/bootstrap.sh $(printf '%q' "$COMPUTE_ID")"
    rsync -az --mkpath -e "$RSYNC_SSH" \
        --exclude=.venv --exclude=artifacts --exclude=__pycache__ \
        --exclude='.test-tmp*' "$HERE/" \
        "$REMOTE:/opt/searchbench/pg_textsearch/benchmarks/scale/"
fi
if [[ "$ACTION" == setup ]]; then exit 0; fi
if [[ "$DATASET" == smoke ]]; then
    DATASET_ARGS="--smoke"
else
    DATASET_ARGS="--dataset $DATASET"
fi
"${SSH[@]}" "$REMOTE" \
    "mkdir -p /srv/searchbench/pg_textsearch-scale && \
     if systemctl is-failed --quiet pgts-benchmark-$RUN_ID; then \
       sudo -A systemctl reset-failed pgts-benchmark-$RUN_ID; \
     fi && \
     sudo -A systemd-run --unit=pgts-benchmark-$RUN_ID \
       --uid=bench --gid=bench \
       --working-directory=/opt/searchbench/pg_textsearch \
       --property=StandardOutput=append:\
/srv/searchbench/pg_textsearch-scale/launch-$RUN_ID.log \
       --property=StandardError=append:\
/srv/searchbench/pg_textsearch-scale/launch-$RUN_ID.log \
       env MSMARCO_ACCEPT_TERMS=$ACCEPT \
       PGTS_NATIVE_SOURCE=$NATIVE_SOURCE \
       bash benchmarks/scale/run.sh $DATASET_ARGS --run-id $RUN_ID all && \
     systemctl is-active pgts-benchmark-$RUN_ID"
printf 'Started %s on %s; native VM expiry is unchanged.\n' "$RUN_ID" "$IP"
