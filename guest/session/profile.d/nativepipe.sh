# NativePipe's display manager runs outside the console login process, so a
# login shell cannot inherit its environment. Attach the shell only when this
# user's live Wayland socket exists; ordinary recovery/root shells stay clean.
_nativepipe_uid="$(id -u 2>/dev/null)"
_nativepipe_runtime="/run/user/${_nativepipe_uid}"
_nativepipe_session="${_nativepipe_runtime}/nativepipe-wayland.env"
_nativepipe_display=
_nativepipe_xdisplay=
_nativepipe_xauthority=
if [ -r "${_nativepipe_session}" ]; then
    _nativepipe_display="$(sed -n 's/^WAYLAND_DISPLAY=//p' "${_nativepipe_session}" | sed -n '1p')"
    _nativepipe_xdisplay="$(sed -n 's/^DISPLAY=//p' "${_nativepipe_session}" | sed -n '1p')"
    _nativepipe_xauthority="$(sed -n 's/^XAUTHORITY=//p' "${_nativepipe_session}" | sed -n '1p')"
fi
case "${_nativepipe_display}" in
    ''|*[!A-Za-z0-9_.-]*) _nativepipe_display= ;;
esac
if [ -n "${_nativepipe_display}" ] && \
    [ -S "${_nativepipe_runtime}/${_nativepipe_display}" ]; then
    if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
        XDG_RUNTIME_DIR="${_nativepipe_runtime}"
        export XDG_RUNTIME_DIR
    fi
    if [ -z "${WAYLAND_DISPLAY:-}" ]; then
        WAYLAND_DISPLAY="${_nativepipe_display}"
        export WAYLAND_DISPLAY
    fi
    XDG_SESSION_TYPE=wayland
    XDG_CURRENT_DESKTOP=NativePipe
    export XDG_SESSION_TYPE XDG_CURRENT_DESKTOP
    case "${_nativepipe_xdisplay}" in
        :[0-9]*|:[0-9]*.[0-9]*)
            case "${_nativepipe_xauthority}" in
                "${_nativepipe_runtime}"/*)
                    if [ -r "${_nativepipe_xauthority}" ]; then
                        DISPLAY="${_nativepipe_xdisplay}"
                        XAUTHORITY="${_nativepipe_xauthority}"
                        export DISPLAY XAUTHORITY
                    fi
                    ;;
            esac
            ;;
    esac

    _nativepipe_preload=/usr/libexec/nativepipe/nativepipe-align-host-blob.so
    _nativepipe_layer=/usr/libexec/nativepipe/nativepipe-vulkan-blob-alignment.so
    _nativepipe_layer_manifest=/etc/vulkan/implicit_layer.d/VkLayer_NATIVEPIPE_blob_alignment.json
    _nativepipe_pagesize="$(getconf PAGESIZE 2>/dev/null || getconf PAGE_SIZE 2>/dev/null || true)"
    if [ -n "${_nativepipe_pagesize}" ] && \
        [ "${_nativepipe_pagesize}" -lt 16384 ] 2>/dev/null && \
        [ -r "${_nativepipe_preload}" ]; then
        case ":${LD_PRELOAD:-}:" in
            *:"${_nativepipe_preload}":*) ;;
            *) LD_PRELOAD="${_nativepipe_preload}${LD_PRELOAD:+:${LD_PRELOAD}}" ;;
        esac
        export LD_PRELOAD
    fi
    _nativepipe_alignment_ready=
    if [ -n "${_nativepipe_pagesize}" ] && \
        [ "${_nativepipe_pagesize}" -lt 16384 ] 2>/dev/null && \
        [ -r "${_nativepipe_layer}" ] && \
        [ -r "${_nativepipe_layer_manifest}" ]; then
        _nativepipe_alignment_ready=1
    fi
    for _nativepipe_icd in /usr/share/vulkan/icd.d/virtio_icd*.json \
        /etc/vulkan/icd.d/virtio_icd*.json; do
        if [ -r "${_nativepipe_icd}" ]; then
            if [ -n "${_nativepipe_alignment_ready}" ]; then
                NATIVEPIPE_BLOB_ALIGNMENT=16384
                export NATIVEPIPE_BLOB_ALIGNMENT
            fi
            break
        fi
    done
    unset _nativepipe_icd
    unset _nativepipe_pagesize
    unset _nativepipe_preload
    unset _nativepipe_layer _nativepipe_layer_manifest _nativepipe_alignment_ready
fi
unset _nativepipe_uid _nativepipe_runtime _nativepipe_session _nativepipe_display
unset _nativepipe_xdisplay _nativepipe_xauthority
