#!/system/bin/sh
#
# joan: turn on dynamic debug for the inter-processor plumbing before the
# remoteprocs come up, so the next ADSP wedge is explained instead of just
# observed. The cmdline already has "ignore_loglevel loglevel=8", so these
# KERN_DEBUG messages reach the console (and therefore pstore/ramoops).
#
# Enable only for an explicitly requested diagnostic boot. Tracing every QMI
# packet and mailbox message continuously adds work to normal UI interactions.
[ "$(getprop persist.vendor.joan.dyndbg)" = 1 ] || exit 0

# dynamic_debug only accepts paths that exist in the compiled code; a wrong
# glob just matches nothing, so this is safe to over-specify.
D=/sys/kernel/debug/dynamic_debug/control

[ -w "$D" ] || exit 0

for q in \
	"file drivers/rpmsg/* +p" \
	"file net/qrtr/* +p" \
	"file drivers/remoteproc/* +p" \
	"file drivers/soc/qcom/* +p" \
	"file drivers/mailbox/* +p" \
	"file drivers/net/wireless/ath/ath10k/* +p" \
	"file drivers/slimbus/* +p" \
	"file drivers/nvmem/* +p" \
	"file drivers/firmware/qcom/* +p"
do
	echo "$q" > "$D" 2>/dev/null
done

# Report what actually got switched on, for the log.
echo "dyndbg: $(grep -c '=p' "$D" 2>/dev/null) calls enabled"
