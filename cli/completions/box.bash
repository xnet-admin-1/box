#!/bin/bash
# Tab completion for the box CLI
# Source this: . /path/to/box.bash
# Or copy to /etc/bash_completion.d/box

_box_completions() {
    local cur prev commands
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    commands="run exec stop rm ps logs inspect images pull up down setup help version"

    # Complete command names
    if [ "$COMP_CWORD" -eq 1 ]; then
        COMPREPLY=($(compgen -W "$commands" -- "$cur"))
        return
    fi

    local cmd="${COMP_WORDS[1]}"

    case "$cmd" in
        run|pull)
            # Complete image names
            if [ "$prev" = "run" ] || [ "$prev" = "pull" ]; then
                local images="ubuntu alpine"
                # Also add locally installed images
                local box_home="${BOX_HOME:-$HOME/box}"
                if [ -d "$box_home/images" ]; then
                    for d in "$box_home/images"/*/; do
                        [ -d "$d" ] && images="$images $(basename "$d")"
                    done
                fi
                COMPREPLY=($(compgen -W "$images" -- "$cur"))
            elif [ "$prev" = "-n" ] || [ "$prev" = "--name" ]; then
                return  # user provides name
            elif [ "$prev" = "-v" ] || [ "$prev" = "--volume" ]; then
                compopt -o nospace
                COMPREPLY=($(compgen -o dirnames -- "$cur"))
            elif [ "$prev" = "-w" ] || [ "$prev" = "--workdir" ]; then
                return  # user provides path
            elif [ "$prev" = "-e" ] || [ "$prev" = "--env" ]; then
                return  # user provides KEY=VALUE
            else
                local flags="-d --detach -n --name -v --volume -e --env -p --port -w --workdir --"
                COMPREPLY=($(compgen -W "$flags" -- "$cur"))
            fi
            ;;
        exec|stop|rm|logs|inspect|setup)
            # Complete container names
            if [ "$prev" = "$cmd" ]; then
                local containers=""
                local box_home="${BOX_HOME:-$HOME/box}"
                if [ -d "$box_home/containers" ]; then
                    for d in "$box_home/containers"/*/; do
                        [ -d "$d" ] || continue
                        local name=$(basename "$d")
                        case "$name" in __boxfile_*) continue ;; esac
                        containers="$containers $name"
                    done
                fi
                COMPREPLY=($(compgen -W "$containers" -- "$cur"))
            fi
            ;;
        up|down)
            # Complete Boxfile paths
            compopt -o default
            COMPREPLY=($(compgen -f -X '!*Boxfile*' -- "$cur"))
            ;;
    esac
}

complete -F _box_completions box
