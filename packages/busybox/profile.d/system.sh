# System Settings

export PATH="/bin:/sbin:/usr/bin:/usr/sbin"
export HOSTNAME=$(cat /etc/hostname)

PS1='\[\e[0;32m\]\u\[\e[m\] \[\e[1;34m\]\w\[\e[m\] \[\e[1;32m\]\$\[\e[m\] \[\e[1;37m\]'
export PS1

TERM="linux"
export TERM

alias ll='ls -al'
loadkmap < /etc/it.bmap
