#!/usr/bin/env bash
# Make Crimson Text the default for text, INI, Markdown, JSON, TOML, XML and empty files, run by the install-default task
#

set -u
readonly SUCCESS=0 INSTALL_FAILED=1
readonly EDIT_ID=$APPLICATION_ID.Edit
# Text, INI, Markdown, JSON, TOML and XML files, as shared-mime-info names them
readonly DEFAULT_TYPES=(text/plain text/markdown application/json application/toml application/xml application/x-zerosize)
# Shell scripts keep their own default, and only middle click opens them in the editor
readonly EDIT_TYPES=(text/x-shellscript)
readonly MIME_ALIASES=/usr/share/mime/aliases

# Prints a file type and its older names, which KDE merges into one list of applications
list_type_names() {
	printf '%s\n' "$1"
	if [[ -r $MIME_ALIASES ]]; then
		awk -v type="$1" '$2 == type { print $1 }' "$MIME_ALIASES"
	fi
	return "$SUCCESS"
}

# Gives every name of the type the same default followed by the editor, since Dolphin opens the second
# application of the merged list on middle click and a different default under an old name would come first
set_type_handlers() {
	local type="$1" default="$2" name
	while IFS= read -r name; do
		xdg-mime default "$default" "$name" || return "$INSTALL_FAILED"
		# KDE ranks its own file above mimeapps.list, which matters where a parent type such as JavaScript
		# brings another default along at the same rank, as it does for JSON
		if command -v kwriteconfig6 >/dev/null; then
			kwriteconfig6 --file kde-mimeapps.list --group 'Default Applications' --key "$name" \
				"$default;$EDIT_ID.desktop;" \
				|| return "$INSTALL_FAILED"
			kwriteconfig6 --file mimeapps.list --group 'Added Associations' --key "$name" \
				"$default;$EDIT_ID.desktop;" || return "$INSTALL_FAILED"
		fi
	done < <(list_type_names "$type")
	return "$SUCCESS"
}

# Opens the file types here on double click and all of them in the editor on middle click in Dolphin
make_default() {
	local type default
	for type in "${DEFAULT_TYPES[@]}"; do
		set_type_handlers "$type" "$APPLICATION_ID.desktop" || return "$INSTALL_FAILED"
	done
	for type in "${EDIT_TYPES[@]}"; do
		default=$(xdg-mime query default "$type")
		set_type_handlers "$type" "${default:-$APPLICATION_ID.desktop}" || return "$INSTALL_FAILED"
	done
	if command -v kbuildsycoca6 >/dev/null; then
		kbuildsycoca6 >/dev/null 2>&1
	else
		printf '%s\n' 'Middle click editing needs KDE, so only the defaults were set.'
	fi
	return "$SUCCESS"
}

make_default || exit "$INSTALL_FAILED"
exit "$SUCCESS"
