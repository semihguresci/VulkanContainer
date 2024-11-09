$env:DISPLAY = "192.168.178.26:0.0"
docker build --pull --rm -f "dockerfile" -t container:latest "." 
docker run --gpus all -it -e DISPLAY=$env:DISPLAY container:latest /bin/bash ./rebuild.sh