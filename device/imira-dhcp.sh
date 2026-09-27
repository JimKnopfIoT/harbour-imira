#!/bin/sh
# imira-dhcp.sh — udhcpc-Hook für imira-connect.sh. Konfiguriert NICHTS
# (Adresse setzt imira-session.sh selbst auf das Gruppen-Interface), schreibt
# nur die Lease als "ip router serverid prefix lease" nach $IMIRA_DHCP_OUT.
case "$1" in
    bound|renew)
        pfx="${mask:-}"
        if [ -z "$pfx" ] && [ -n "${subnet:-}" ]; then
            pfx=$(echo "$subnet" | awk -F. '{n=0; for(i=1;i<=4;i++){b=$i; while(b>0){n+=b%2; b=int(b/2)}}; print n}')
        fi
        r="${router:-}"; r="${r%% *}"
        # "-" als Platzhalter, damit die Felder beim Zerlegen nicht verrutschen.
        echo "${ip:--} ${r:--} ${serverid:--} ${pfx:-24} ${lease:--}" \
            > "${IMIRA_DHCP_OUT:-/tmp/imira-dhcp.lease}"
        ;;
esac
exit 0
