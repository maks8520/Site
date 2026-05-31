docker run --rm -v $(pwd):/work -w /work/vostochnaya_baza_s3 espressif/idf:release-v5.1 /bin/bash -c ". \$IDF_PATH/export.sh && idf.py set-target esp32s3 && idf.py build"
