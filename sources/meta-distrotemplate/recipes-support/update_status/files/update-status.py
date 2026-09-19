#!/usr/bin/python3

import subprocess

# Get the uboot env variables 
env_variables = subprocess.run(['fw_printenv'], stdout=subprocess.PIPE).stdout.decode('utf-8')
env_variables = env_variables.splitlines()

env = {}

for line in env_variables:
  item = line.split("=")
  env[item[0]] = item[1]

# Check if last upgrade failed
if env['upgrade_available'] == '0' and env['last_upgrade_failed'] == '1':
  print(f"LAST UPGRADE FAILED at {env['last_failed_slot']}")
  subprocess.run(['fw_setenv', 'bootcount', '0'])
  # Here we can consider recovering last update slot with current running one

# Check if we are on a successful updated boot
if env['upgrade_available'] == '1' and env['last_upgrade_failed'] == '0' and env['last_failed_slot'] == 'none':
  print("We are on a successfull updated boot")
  subprocess.run(['fw_setenv', 'upgrade_available', '0'])
  subprocess.run(['fw_setenv', 'bootcount', '0'])
  # Here we can consider copying currently running slot into inactice outdated one