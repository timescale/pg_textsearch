#!/usr/bin/env python3
"""Provision a dedicated Azure DevTest Labs VM with native auto-expiration."""

import argparse
import datetime as dt
import ipaddress
import json
import pathlib
import subprocess
import sys
import tempfile


MARKER = "pg_textsearch-scale"


def template(location, size, public_key, ssh_cidr, expires):
    network = ipaddress.ip_network(ssh_cidr, strict=True)
    if network.version != 4 or network.prefixlen != 32:
        raise ValueError("SSH must be restricted to one explicit IPv4 /32")
    if not public_key.startswith(("ssh-ed25519 ", "ssh-rsa ")):
        raise ValueError("An SSH public key is required")
    dt.datetime.strptime(expires, "%Y-%m-%dT%H:%M:%SZ")

    def rid(kind, *names):
        names = ", ".join(f"'{name}'" for name in names)
        return f"[resourceId('{kind}', {names})]"

    lab = rid("Microsoft.DevTestLab/labs", "searchbench")
    nsg = rid("Microsoft.Network/networkSecurityGroups", "benchmark-nsg")
    vnet = rid("Microsoft.Network/virtualNetworks", "benchmark-vnet")
    subnet = rid("Microsoft.Network/virtualNetworks/subnets",
                 "benchmark-vnet", "default")
    labnet = rid("Microsoft.DevTestLab/labs/virtualNetworks",
                 "searchbench", "benchmark-vnet")
    resources = [{
        "type": "Microsoft.Network/networkSecurityGroups",
        "apiVersion": "2023-09-01", "name": "benchmark-nsg",
        "location": location,
        "properties": {"securityRules": [{
            "name": "ssh-from-controller",
            "properties": {
                "priority": 100, "direction": "Inbound", "access": "Allow",
                "protocol": "Tcp", "sourcePortRange": "*",
                "destinationPortRange": "22",
                "sourceAddressPrefix": ssh_cidr,
                "destinationAddressPrefix": "*",
            },
        }, {
            "name": "deny-other-inbound",
            "properties": {
                "priority": 200, "direction": "Inbound", "access": "Deny",
                "protocol": "*", "sourcePortRange": "*",
                "destinationPortRange": "*", "sourceAddressPrefix": "*",
                "destinationAddressPrefix": "*",
            },
        }]},
    }, {
        "type": "Microsoft.Network/virtualNetworks",
        "apiVersion": "2023-09-01", "name": "benchmark-vnet",
        "location": location, "dependsOn": [nsg],
        "properties": {
            "addressSpace": {"addressPrefixes": ["10.73.0.0/16"]},
            "subnets": [{"name": "default", "properties": {
                "addressPrefix": "10.73.0.0/24",
                "networkSecurityGroup": {"id": nsg},
                "defaultOutboundAccess": True,
            }}],
        },
    }, {
        "type": "Microsoft.DevTestLab/labs", "apiVersion": "2018-09-15",
        "name": "searchbench", "location": location,
        "tags": {"managed-by": MARKER, "expires": expires},
        "properties": {
            "labStorageType": "Premium",
            "premiumDataDisks": "Enabled",
            "environmentPermission": "Contributor",
        },
    }, {
        "type": "Microsoft.DevTestLab/labs/virtualNetworks",
        "apiVersion": "2018-09-15",
        "name": "searchbench/benchmark-vnet", "location": location,
        "dependsOn": [lab, vnet],
        "properties": {
            "externalProviderResourceId": vnet,
            "subnetOverrides": [{
                "labSubnetName": "default", "resourceId": subnet,
                "useInVmCreationPermission": "Allow",
                "usePublicIpAddressPermission": "Allow",
            }],
        },
    }, {
        "type": "Microsoft.DevTestLab/labs/virtualMachines",
        "apiVersion": "2018-09-15",
        "name": "searchbench/searchbench", "location": location,
        "tags": {"managed-by": MARKER, "expires": expires},
        "dependsOn": [labnet],
        "properties": {
            "userName": "bench", "sshKey": public_key,
            "isAuthenticationWithSshKey": True,
            "size": size, "storageType": "Premium", "osType": "Linux",
            "allowClaim": False, "disallowPublicIpAddress": False,
            "expirationDate": expires,
            "labVirtualNetworkId": labnet, "labSubnetName": "default",
            "galleryImageReference": {
                "publisher": "Canonical", "offer": "ubuntu-24_04-lts",
                "sku": "server", "osType": "Linux",
                "version": "24.04.202611170",
            },
            "scheduleParameters": [{
                "name": "LabVmsShutdown",
                "properties": {
                    "status": "Disabled", "taskType": "LabVmsShutdownTask",
                    "dailyRecurrence": {"time": "1900"},
                    "timeZoneId": "UTC",
                    "notificationSettings": {"status": "Disabled"},
                },
            }],
        },
    }]
    return {
        "$schema": ("https://schema.management.azure.com/schemas/"
                    "2019-04-01/deploymentTemplate.json#"),
        "contentVersion": "1.0.0.0", "resources": resources,
    }


def az(subscription, *args):
    try:
        result = subprocess.run(
            ["az", *args, "--subscription", subscription, "-o", "json",
             "--only-show-errors"], check=True, text=True, capture_output=True,
        )
    except subprocess.CalledProcessError as error:
        if error.stderr:
            print(error.stderr.rstrip(), file=sys.stderr)
        raise
    return json.loads(result.stdout) if result.stdout.strip() else None


def describe_vm(subscription, group):
    vm_id = (f"/subscriptions/{subscription}/resourceGroups/{group}/providers/"
             "Microsoft.DevTestLab/labs/searchbench/virtualmachines/searchbench")
    vm = az(subscription, "rest", "--method", "get", "--url",
            f"https://management.azure.com{vm_id}?api-version=2018-09-15")
    properties = vm["properties"]
    result = {key: properties.get(key) for key in (
        "computeId", "expirationDate", "provisioningState", "size",
    )}
    result["networkInterface"] = dict(properties.get("networkInterface") or {})
    if properties.get("computeId"):
        compute_group = properties["computeId"].split("/")[4]
        addresses = az(subscription, "vm", "list-ip-addresses",
                       "-g", compute_group, "-n", "searchbench")
        public = addresses[0]["virtualMachine"]["network"]["publicIpAddresses"]
        if public:
            result["networkInterface"]["publicIpAddress"] = public[0]["ipAddress"]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["plan", "create", "status", "destroy"])
    parser.add_argument("--subscription", required=True)
    parser.add_argument("--group", required=True)
    parser.add_argument("--location", default="westus2")
    parser.add_argument("--size", default="Standard_L32as_v3")
    parser.add_argument("--ssh-key", type=pathlib.Path)
    parser.add_argument("--ssh-cidr")
    parser.add_argument("--days", type=int, default=14)
    parser.add_argument("--expires", help="UTC ISO timestamp (for expiry tests)")
    parser.add_argument("--resume", action="store_true",
                        help="Retry a marked deployment using its saved expiry")
    parser.add_argument("--state", type=pathlib.Path,
                        default=pathlib.Path("benchmarks/results/azure.json"))
    args = parser.parse_args()
    if "searchbench-" not in args.group:
        parser.error("Use a dedicated group name containing 'searchbench-'")
    if args.action == "status":
        vm = describe_vm(args.subscription, args.group)
        if args.state.exists():
            saved = json.loads(args.state.read_text())
            if (saved["subscription"] == args.subscription
                    and saved["group"] == args.group):
                saved.update(
                    status=("provisioned" if vm["provisioningState"] == "Succeeded"
                            else "provisioning"),
                    vm=vm,
                )
                args.state.write_text(json.dumps(saved, indent=2) + "\n")
        print(json.dumps(vm, indent=2))
        return
    if args.action == "destroy":
        group = az(args.subscription, "group", "show", "-n", args.group)
        if group.get("tags", {}).get("managed-by") != MARKER:
            parser.error("Refusing to delete an unrecognized resource group")
        # DevTest Labs places its VM in a separate, service-created group.
        vm_id = (f"/subscriptions/{args.subscription}/resourceGroups/"
                 f"{args.group}/providers/Microsoft.DevTestLab/labs/"
                 "searchbench/virtualMachines/searchbench")
        resources = az(args.subscription, "resource", "list",
                       "-g", args.group)
        if any(r["id"].lower() == vm_id.lower() for r in resources):
            az(args.subscription, "resource", "delete", "--ids", vm_id,
               "--api-version", "2018-09-15")
        # The lab RP releases its own storage, vault, and network locks.
        lab_id = vm_id.rsplit("/virtualMachines/", 1)[0]
        if any(r["id"].lower() == lab_id.lower() for r in resources):
            az(args.subscription, "resource", "delete", "--ids", lab_id,
               "--api-version", "2018-09-15")
        az(args.subscription, "group", "delete", "-n", args.group, "--yes")
        return
    if not args.ssh_key or not args.ssh_cidr:
        parser.error("--ssh-key and --ssh-cidr are required")
    if not 1 <= args.days <= 14:
        parser.error("--days must be between 1 and 14")
    previous = None
    if args.resume:
        previous = json.loads(args.state.read_text())
        if (previous["subscription"] != args.subscription
                or previous["group"] != args.group):
            parser.error("Resume state belongs to another subscription/group")
        args.expires = previous["expires"]
        args.size = previous["size"]
        args.location = previous["location"]
    now = dt.datetime.now(dt.timezone.utc)
    expires = args.expires or (now + dt.timedelta(days=args.days)).strftime(
        "%Y-%m-%dT%H:%M:%SZ")
    deadline = dt.datetime.strptime(expires, "%Y-%m-%dT%H:%M:%SZ").replace(
        tzinfo=dt.timezone.utc)
    if not now < deadline <= now + dt.timedelta(days=14):
        parser.error("Expiration must be in the next 14 days")
    deployment = template(args.location, args.size,
                          args.ssh_key.read_text().strip(),
                          args.ssh_cidr, expires)
    if args.action == "plan":
        print(json.dumps(deployment, indent=2))
        return
    if az(args.subscription, "group", "exists", "-n", args.group):
        group = az(args.subscription, "group", "show", "-n", args.group)
        if not previous or group.get("tags", {}).get("managed-by") != MARKER:
            parser.error("Group exists; use its matching state with --resume")
    else:
        az(args.subscription, "group", "create", "-n", args.group,
           "--location", args.location, "--tags",
           f"managed-by={MARKER}", f"expires={expires}")
    args.state.parent.mkdir(parents=True, exist_ok=True)
    state = {"subscription": args.subscription, "group": args.group,
             "expires": expires, "location": args.location,
             "size": args.size, "status": "deploying"}
    args.state.write_text(json.dumps(state, indent=2) + "\n")
    args.state.chmod(0o600)
    with tempfile.NamedTemporaryFile(mode="w", suffix=".json") as handle:
        json.dump(deployment, handle)
        handle.flush()
        try:
            az(args.subscription, "deployment", "group", "create",
               "-g", args.group, "-n", "benchmark",
               "--template-file", handle.name)
        except subprocess.CalledProcessError:
            print(f"Deployment failed; inspect or destroy {args.group}.")
            raise
    state.update(status="provisioned",
                 vm=describe_vm(args.subscription, args.group))
    args.state.write_text(json.dumps(state, indent=2) + "\n")
    print(json.dumps(state, indent=2))


if __name__ == "__main__":
    main()
