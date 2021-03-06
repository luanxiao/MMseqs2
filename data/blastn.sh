#!/bin/sh -e
# Sequence search workflow script
fail() {
    echo "Error: $1"
    exit 1
}

notExists() {
	[ ! -f "$1" ]
}

#pre processing
[ -z "$MMSEQS" ] && echo "Please set the environment variable \$MMSEQS to your MMSEQS binary." && exit 1;
# check amount of input variables
[ "$#" -ne 4 ] && echo "Please provide <queryDB> <targetDB> <outDB> <tmp>" && exit 1;
# check if files exists
[ ! -f "$1" ] &&  echo "$1 not found!" && exit 1;
[ ! -f "$2" ] &&  echo "$2 not found!" && exit 1;
[   -f "$3" ] &&  echo "$3 exists already!" && exit 1;
[ ! -d "$4" ] &&  echo "tmp directory $4 not found!" && mkdir -p "$4";


QUERY="$1"
TARGET="$2"
TMP_PATH="$4"

if [ -n "$NEEDTARGETSPLIT" ]; then
    if notExists "$TMP_PATH/target_seqs_split"; then
        # shellcheck disable=SC2086
        "$MMSEQS" splitsequence "$TARGET" "$TMP_PATH/target_seqs_split" ${SPLITSEQUENCE_PAR}  \
            || fail "Split sequence died"
    fi
    TARGET="$TMP_PATH/target_seqs_split"
fi

if [ -n "$EXTRACTFRAMES" ]; then
    if notExists "$TMP_PATH/query_seqs"; then
        # shellcheck disable=SC2086
        "$MMSEQS" extractframes "$QUERY" "$TMP_PATH/query_seqs" ${EXTRACT_FRAMES_PAR}  \
            || fail "Extractframes died"
    fi
    QUERY="$TMP_PATH/query_seqs"
fi

if [ -n "$NEEDQUERYSPLIT" ]; then
    if notExists "$TMP_PATH/query_seqs_split"; then
        # shellcheck disable=SC2086
        "$MMSEQS" splitsequence "$QUERY" "$TMP_PATH/query_seqs_split" ${SPLITSEQUENCE_PAR}  \
        || fail "Split sequence died"
    fi
    QUERY="$TMP_PATH/query_seqs_split"
fi

mkdir -p "$4/search"
if notExists "$4/aln"; then
    # shellcheck disable=SC2086
    "$SEARCH" "${QUERY}" "${TARGET}" "$4/aln" "$4/search" ${SEARCH_PAR} \
        || fail "Search step died"
fi

if notExists "$4/aln_offset"; then
    # shellcheck disable=SC2086
    "$MMSEQS" offsetalignment "$1" "${QUERY}" "$2" "${TARGET}" "$4/aln"  "$4/aln_offset" ${OFFSETALIGNMENT_PAR} \
        || fail "Offset step died"
fi

(mv -f "$4/aln_offset" "$3" && mv -f "$4/aln_offset.index" "$3.index") \
    || fail "Could not move result to $3"

if [ -n "$REMOVE_TMP" ]; then
  echo "Remove temporary files"
  rm -f "$4/q_orfs"    "$4/q_orfs.index"    "$4/q_orfs.dbtype"
  rm -f "$4/q_orfs_aa" "$4/q_orfs_aa.index" "$4/q_orfs_aa.dbtype"
  rm -f "$4/t_orfs"    "$4/t_orfs.index"    "$4/t_orfs.dbtype"
  rm -f "$4/t_orfs_aa" "$4/t_orfs_aa.index" "$4/t_orfs_aa.dbtype"
fi

