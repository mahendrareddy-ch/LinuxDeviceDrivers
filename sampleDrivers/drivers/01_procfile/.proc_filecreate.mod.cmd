savedcmd_proc_filecreate.mod := printf '%s\n'   proc_filecreate.o | awk '!x[$$0]++ { print("./"$$0) }' > proc_filecreate.mod
