#!/bin/sh
# /etc/acpi/handler.sh for the t410dash kiosk.
#
# acpid calls this for every ACPI event. The kiosk must stay on: closing
# the lid or (un)plugging AC never suspends, hibernates or blanks.
# Only the power button does anything (a clean poweroff).
#
# $1 = event group/action, e.g. "button/lid", "ac_adapter", "button/power"

case "$1" in
	button/power)
		/sbin/poweroff
		;;
	button/lid|ac_adapter|battery|button/sleep|button/suspend)
		# deliberately ignored
		;;
	*)
		;;
esac

exit 0
