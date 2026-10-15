#!/bin/bash
# guest-click.sh X Y [click|dclick|move]
#
# Runs on the Haiku machine: moves the BeBox guest's mouse pointer to X,Y
# and clicks there. The app must have captured the mouse. The pointer is
# moved in small relative steps through BeNC's sendinput (large ones are
# accelerated by BeOS), closing the loop on the app's Pointer property.
SENDINPUT=${SENDINPUT:-/boot/home/BeNC/tests/haiku/sendinput}
SIGNATURE=application/x-vnd.BeBox-Emulator
# the last host position sent, so that the motion continues from there
STATE=/tmp/bebox-guest-click.pos
tx=$1
ty=$2
what=${3:-click}

send() {
	$SENDINPUT $SIGNATURE BeBox view:screen "$@"
}

clamp() {
	local v=$1
	[ $v -gt 6 ] && v=6
	[ $v -lt -6 ] && v=-6
	echo $v
}

if [ -f $STATE ]; then
	read hx hy < $STATE
else
	hx=400
	hy=300
fi
for i in $(seq 60); do
	read cx cy < <(hey BeBox get Pointer of Window BeBox \
		| sed -n 's/.*BPoint(\([-0-9.]*\), \([-0-9.]*\)).*/\1 \2/p')
	cx=${cx%.*}
	cy=${cy%.*}
	dx=$((tx - cx))
	dy=$((ty - cy))
	[ $dx -eq 0 ] && [ $dy -eq 0 ] && break
	moves=""
	for s in $(seq 12); do
		[ $dx -eq 0 ] && [ $dy -eq 0 ] && break
		sx=$(clamp $dx)
		sy=$(clamp $dy)
		hx=$((hx + sx))
		hy=$((hy + sy))
		dx=$((dx - sx))
		dy=$((dy - sy))
		moves="$moves move:$hx,$hy wait:10"
	done
	send $moves
	sleep 0.25
done
echo "$hx $hy" > $STATE
case $what in
	click)
		send press:$hx,$hy,1 wait:60 release:$hx,$hy
		;;
	dclick)
		send press:$hx,$hy,1 wait:40 release:$hx,$hy wait:60 \
			press:$hx,$hy,1 wait:40 release:$hx,$hy
		;;
esac
echo "pointer at $cx,$cy"
