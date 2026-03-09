savedcmd_the_usctm.mod := printf '%s\n'   usctm.o ./lib/vtpmo.o | awk '!x[$$0]++ { print("./"$$0) }' > the_usctm.mod
