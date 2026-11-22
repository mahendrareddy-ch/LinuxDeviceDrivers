savedcmd_my_usbdriver.mod := printf '%s\n'   my_usbdriver.o | awk '!x[$$0]++ { print("./"$$0) }' > my_usbdriver.mod
