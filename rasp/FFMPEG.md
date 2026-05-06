To stream audio use the following command:
```bash
ffmpeg -f avfoundation -i ":1" \
-acodec libmp3lame -b:a 128k \
-bufsize 32k \
-fflags nobuffer \
-flags low_delay \
-flush_packets 1 \
-max_delay 0 \
-f mp3 udp://127.0.0.1:3000
```

To check the devices available:
```bash
ffmpeg -f avfoundation -list_devices true -i ""
```
