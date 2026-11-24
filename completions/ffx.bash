# bash completion for ffx
_ffx() {
	local cur prev cword
	cur=${COMP_WORDS[COMP_CWORD]}
	prev=${COMP_WORDS[COMP_CWORD - 1]}
	cword=$COMP_CWORD

	local cmds="run install uninstall list doctor icons setup version help"
	local opts="--blur --no-blur --radius --icon --name --id --profile --args --desktop --force --shadow --no-menu --debug --help"

	case $prev in
	--icon)
		local presets
		presets=$(ffx icons 2>/dev/null | awk 'NF==2{print $1}')
		COMPREPLY=($(compgen -W "$presets" -- "$cur"))
		return
		;;
	--profile)
		COMPREPLY=($(compgen -d -- "$cur"))
		return
		;;
	--radius | --name | --id | --args) return ;;
	esac

	if [ "$cword" -eq 1 ]; then
		COMPREPLY=($(compgen -W "$cmds" -- "$cur"))
		return
	fi
	if [ "$cword" -eq 2 ] && [ "${COMP_WORDS[1]}" = run ]; then
		COMPREPLY=($(compgen -W "$opts" -- "$cur"))
		return
	fi
	COMPREPLY=($(compgen -W "$opts $cmds" -- "$cur"))
}
complete -F _ffx ffx
