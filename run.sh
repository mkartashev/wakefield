PWD=`pwd`
LOGGING="--logger-scopes=wakefield --log=log.txt"
weston --socket=wayland-42 \
    --output-count=2 \
    --width=800 --height=700 --use-pixman \
    $LOGGING \
    --modules="$PWD/libwakefield.so"
