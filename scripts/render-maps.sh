#!/bin/sh
# Render the current text map data into the named volume served by Apache.

set -eu

mud_root=${EMPIRE_DIR:-/opt/empiremud}
output_dir=${EMPIRE_MAP_DIR:-/srv/maps}
php_map="${mud_root}/php/map.php"
viewer_source="${mud_root}/php/map-viewer.php"

mkdir -p "${output_dir}"

temporary=

cleanup() {
    if [ -n "${temporary}" ]; then
        rm -f "${temporary}"
    fi
}

trap cleanup 0
trap 'exit 1' HUP INT TERM

new_temporary() {
    destination=$1
    temporary=$(mktemp "${destination}.tmp.XXXXXX")
}

publish_temporary() {
    destination=$1

    chmod 0644 "${temporary}"
    mv "${temporary}" "${destination}"
    temporary=
}

render_atomic() {
    map_type=$1
    destination=$2

    new_temporary "${destination}"

    if [ -n "${map_type}" ]; then
        if ! php "${php_map}" "${map_type}" > "${temporary}"; then
            echo "Map renderer failed for ${destination}." >&2
            return 1
        fi
    elif ! php "${php_map}" > "${temporary}"; then
        echo "Map renderer failed for ${destination}." >&2
        return 1
    fi

    # The dollar-prefixed variables below are PHP, not shell expansions.
    # shellcheck disable=SC2016
    if ! php -r '
        $file = $argv[1];
        $info = @getimagesize($file);
        $image = ($info !== false && $info[2] === IMAGETYPE_PNG)
            ? @imagecreatefrompng($file)
            : false;
        if ($image === false) {
            fwrite(STDERR, "Renderer output is not a valid PNG: {$file}\n");
            exit(1);
        }
        imagedestroy($image);
    ' "${temporary}"; then
        return 1
    fi

    publish_temporary "${destination}"
}

if [ -s "${mud_root}/data/map.txt" ]; then
    render_atomic "" "${output_dir}/map.png"
else
    render_atomic generated "${output_dir}/map.png"
fi

if [ -s "${mud_root}/data/map-political.txt" ]; then
    render_atomic political "${output_dir}/map-political.png"
else
    # Before the first game boot there is no political map yet. Use the
    # generated terrain map so the viewer remains functional.
    render_atomic generated "${output_dir}/map-political.png"
fi

if [ -s "${mud_root}/data/map-cities.txt" ]; then
    new_temporary "${output_dir}/map-cities.txt"
    cp "${mud_root}/data/map-cities.txt" "${temporary}"
else
    new_temporary "${output_dir}/map-cities.txt"
    printf '$\n' > "${temporary}"
fi
publish_temporary "${output_dir}/map-cities.txt"

# The stock viewer deliberately contains a deployment placeholder. Configure
# only the copy in the web volume and leave the upstream PHP source untouched.
new_temporary "${output_dir}/map-viewer.php"
sed "s#\$pathToData = '/path/to/empireMUD/data/';#\$pathToData = '/var/www/html/';#" \
    "${viewer_source}" > "${temporary}"
publish_temporary "${output_dir}/map-viewer.php"

echo "Rendered maps in ${output_dir}."
