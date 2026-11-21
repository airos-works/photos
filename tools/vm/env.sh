# Sourced by the VM scripts. An instance is a name with its own overlay,
# ports and QMP socket, so several can run at once:
#   VM_NAME=main   SSH 2243  VNC :23 (5923)
#   VM_NAME=ptp    SSH 2244  VNC :24
#   VM_NAME=faces  SSH 2245  VNC :25
# The base image (base/airos.img, an air/OS x86_64 image with an SSH key put
# in by prep-image.sh) is shared and never written.
PHOTOS_VM_WORK=${PHOTOS_VM_WORK:-/data3/airos/photos-work}
VM_NAME=${VM_NAME:-main}
case $VM_NAME in
	main) VM_INDEX=0 ;;
	ptp) VM_INDEX=1 ;;
	faces) VM_INDEX=2 ;;
	*) VM_INDEX=${VM_INDEX:?unknown VM_NAME: set VM_INDEX} ;;
esac
VM_SSH_PORT=$((2243 + VM_INDEX))
VM_VNC_DISPLAY=$((23 + VM_INDEX))
VM_BASE=$PHOTOS_VM_WORK/base
VM_DIR=$PHOTOS_VM_WORK/vm-$VM_NAME
