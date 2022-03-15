PWD=`pwd`
LOGGING="--logger-scopes=wakefield --log=log.txt"
weston -B wayland-backend.so --socket=wayland-test \
    --width=800 --height=600 --use-pixman \
    --socket=wayland-42 \
    $LOGGING \
    --modules="$PWD/libwakefield.so"
