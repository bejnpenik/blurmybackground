# blurmybackground
BlurMyBackground is utillity for bluring your desktop background picture when there are clients open/visible.

![]( blurmybackground/blurmybackground.giff )

Clone:
git clone https://github.com/bejnpenik/blurmybackground.git

Compile:
cd blurmybackground
gcc -o blurmybackground blurmybackground.c `pkg-config --cflags --libs MagickWand cairo xcb xcb-ewmh`

Install:

cp blurmybackground ~/.bin/ 
