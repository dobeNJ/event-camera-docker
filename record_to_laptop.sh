#!/bin/bash

LAPTOP_RECORD_DIR="/prophesee_recording"

DATE_TIME=$(date +"%d-%m-%Y_%H-%M-%S")

SESSION_NAME="recording_$DATE_TIME"
SESSION_DIR="$LAPTOP_RECORD_DIR/$SESSION_NAME"

OUTPUT_FILE="$SESSION_DIR/events_$DATE_TIME"

mkdir -p "$SESSION_DIR"
chmod 777 "$SESSION_DIR"

echo "Recording folder on laptop:"
echo "$SESSION_DIR"

echo ""
echo "Output file will be:"
echo "$OUTPUT_FILE.raw"

echo ""
echo "Starting Prophesee viewer..."
echo "Click the CD Events window."
echo "Press SPACE to start recording."
echo "Press SPACE again to stop recording."
echo "Press q to quit."
echo ""

prophesee_viewer -o "$OUTPUT_FILE"

echo ""
echo "Viewer closed."
echo "Files saved:"
ls -lh "$SESSION_DIR"
