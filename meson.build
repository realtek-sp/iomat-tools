project('iomat-tools', 'c')

# Define and install the executables
executables = [
  executable('iomat-update', 'iomat-update.c', install: true, install_dir: '/usr/bin'),
  executable('iomat-mem', 'iomat-mem.c', install: true, install_dir: '/usr/bin')
]

# Install the EC main image firmware. Kept with a fixed name so iomat-update
# can reference a stable path; firmware version history is tracked via git.
install_data('EC_Main_Image.bin',
  install_dir: get_option('datadir') / 'iomat-tools')
