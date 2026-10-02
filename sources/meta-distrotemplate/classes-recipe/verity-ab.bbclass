# A/B dm-verity rootfs booted through U-Boot extlinux.
#
# The extlinux label of each slot builds its dm-verity table from the U-Boot
# variables verity_sectors_<slot> and verity_args_<slot>, <slot> being "a" or
# "b". This class seeds them for both slots in the factory U-Boot environment
# and gives sw-description the values of this build as @@VERITY_DATA_SECTORS@@
# and @@VERITY_TABLE_ARGS@@.
#
# Next to the image_types_verity outputs it deploys:
#   ${IMAGE_LINK_NAME}.ext4.verity.gz  rootfs and hash tree, for SWUpdate
#   ${IMAGE_LINK_NAME}.u-boot.env      factory U-Boot environment, for wic

IMAGE_CLASSES += "image_types_verity"
IMAGE_FSTYPES += "verity"

# ext4 cannot be mounted on a dm-verity device whose blocks are larger than
# its own.
EXTRA_IMAGECMD:ext4 = "-i 4096 -b ${VERITY_BLOCK_SIZE}"

def verity_ab_params(path):
    with open(path) as f:
        return dict(line.split('=', 1) for line in f.read().splitlines() if '=' in line)

def verity_ab_table_args(params):
    data_blocks = int(params['VERITY_DATA_BLOCKS'])
    data_block_size = int(params['VERITY_DATA_BLOCK_SIZE'])
    hash_block_size = int(params['VERITY_HASH_BLOCK_SIZE'])
    # image_types_verity stores the hash tree right after the data blocks.
    hash_start_block = data_blocks * data_block_size // hash_block_size
    return ' '.join([
        str(data_block_size),
        str(hash_block_size),
        str(data_blocks),
        str(hash_start_block),
        params['VERITY_HASH_ALGORITHM'],
        params['VERITY_ROOT_HASH'],
        params['VERITY_SALT'],
        '1 ignore_zero_blocks',
    ])

python do_verity_ab() {
    import os
    import subprocess

    def link(target, name):
        if os.path.lexists(name):
            os.remove(name)
        os.symlink(os.path.basename(target), name)

    deploy_dir = d.getVar('DEPLOY_DIR_IMAGE')
    imgdeploydir = d.getVar('IMGDEPLOYDIR')
    input_image = d.getVar('VERITY_INPUT_IMAGE')
    params = verity_ab_params(input_image + '.verity-params')

    verity_link = input_image + d.getVar('VERITY_IMAGE_SUFFIX')
    verity = os.path.realpath(verity_link)
    subprocess.run('gzip -f -9 -n -c --rsyncable %s > %s.gz' % (verity, verity), shell=True, check=True)
    link(verity + '.gz', verity_link + '.gz')

    with open(os.path.join(deploy_dir, 'u-boot-initial-env')) as f:
        env = f.read().splitlines()
    for slot in ('a', 'b'):
        env.append('verity_sectors_%s=%s' % (slot, params['VERITY_DATA_SECTORS']))
        env.append('verity_args_%s=%s' % (slot, verity_ab_table_args(params)))
    env_txt = os.path.join(d.getVar('WORKDIR'), 'u-boot-verity-env.txt')
    with open(env_txt, 'w') as f:
        f.write('\n'.join(env) + '\n')

    # The image must have the environment size U-Boot and libubootenv use.
    with open(os.path.join(deploy_dir, 'fw_env.config')) as f:
        env_size = f.read().split()[2]
    env_image = os.path.join(imgdeploydir, d.getVar('IMAGE_NAME') + '.u-boot.env')
    subprocess.run(['mkenvimage', '-s', env_size, '-o', env_image, env_txt], check=True)
    link(env_image, os.path.join(imgdeploydir, d.getVar('IMAGE_LINK_NAME') + '.u-boot.env'))
}
addtask verity_ab after do_image_verity before do_image_complete
do_verity_ab[depends] += "u-boot-mkenvimage-native:do_populate_sysroot virtual/bootloader:do_deploy"
do_image_wic[depends] += "${PN}:do_verity_ab"

python verity_ab_swu_vars() {
    import os

    params = verity_ab_params(os.path.join(d.getVar('DEPLOY_DIR_IMAGE'),
        '%s.%s.verity-params' % (d.getVar('IMAGE_LINK_NAME'), d.getVar('VERITY_IMAGE_FSTYPE'))))
    d.setVar('VERITY_DATA_SECTORS', params['VERITY_DATA_SECTORS'])
    d.setVar('VERITY_TABLE_ARGS', verity_ab_table_args(params))
}
do_swuimage[prefuncs] += "verity_ab_swu_vars"
