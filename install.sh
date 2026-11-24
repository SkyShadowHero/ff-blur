#!/bin/sh
# ffx 的快捷入口。
#
#   ./install.sh              交互菜单（推荐）：执行 / 还原 / 诊断
#   ./install.sh menu         同上
#   ./install.sh direct       非交互：接管 firefox 命令（等价 ffx install --id firefox --shadow --force）
#   ./install.sh uninstall    非交互：全部还原（等价 ffx uninstall all）
#   ./install.sh <其它参数>   原样转给 ffx
set -eu
here=$(dirname "$(readlink -f "$0")")
if [ $# -eq 0 ]; then
	exec "$here/ffx" menu
fi
case $1 in
menu) shift; exec "$here/ffx" menu "$@" ;;
direct) shift; exec "$here/ffx" install --id firefox --shadow --force "$@" ;;
uninstall) shift; exec "$here/ffx" uninstall all "$@" ;;
-h | --help | help)
	echo "./install.sh 是 ffx 的入口，命令行用法见 ffx --help"
	;;
*) exec "$here/ffx" "$@" ;;
esac
