savedcmd_proc_file_readwrite.mod := printf '%s\n'   proc_file_readwrite.o | awk '!x[$$0]++ { print("./"$$0) }' > proc_file_readwrite.mod
