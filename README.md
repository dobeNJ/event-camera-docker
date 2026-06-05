# event-camera-docker
This Dockerfile provides a build environment for the **Prophesee EVALUATION KIT - Gen3S HVGA-EM** event camera. It contains all the dependencies necessary to run the **Metavision SDK 1.4.1** and monitor events from the camera.

## Dependencies

- [Docker](https://www.docker.com/products/docker-desktop/) (to run the container)
- **USB access** (for camera communication)

### **X11 Display Server** (to display GUI applications)
#### For Linux (ubuntu 20.04)
##### Install X11 packages (if not already installed)
  ```bash
  sudo apt-get update
  sudo apt-get install -y xorg xauth x11-apps
```
##### Allow Docker to access the X11 server
This command allows Docker containers to access your display.
```bash
xhost +local:docker
```
##### Verify the DISPLAY variable in your host machine
```bash
echo $DISPLAY
```
If the output is empty or incorrect, set it manually:
```bash
export DISPLAY=:0

```

## Pull the Docker Image

Alternatively, you can build the image from the Dockerfile in this repository

```bash
git clone 
cd event-camera-docker
docker build -t event-camera .
```

## Running the docker container 

This command will give you an interactive shell inside the container. You can then run commands like metavision_vibration_monitoring_demo.

```bash
sudo docker run -it --rm --net=host --env="DISPLAY" --volume="$HOME/.Xauthority:/root/.Xauthority:rw" --privileged --device /dev/bus/usb:/dev/bus/usb custom-event-camera-image /bin/bash
```
```bash
sudo docker run -it --rm --name test2 --net=host --env="DISPLAY" --volume="$HOME/.Xauthority:/root/.Xauthority:rw"  --volume /Documents/Event-analysis/Camera:/workspace--privileged --device /dev/bus/usb:/dev/bus/usb custom-event-camera-image /bin/bash
```

## start docker and save in the computer
The the file are saved in the mounted volume in the line --volume "$HOME/prophesee_recording:/prophesee_recording" \
If I want to saved in another disk I can mount the volume is the same way as : --volume "/media/$USER/Extreme SSD/prophesee_recording:/prophesee_recording" \
But first I need to creat the folder : 

```bash
mkdir -p "/media/$USER/Extreme SSD/prophesee_recording"
chmod 777 "/media/$USER/Extreme SSD/prophesee_recording"
```
And then to creat container with name test2
```bash
sudo docker run -it --rm \
  --name test2 \
  --net=host \
  --env DISPLAY=$DISPLAY \
  --volume "$HOME/.Xauthority:/root/.Xauthority:rw" \
  --volume "$HOME/prophesee_recording:/prophesee_recording" \
  --privileged \
  --device /dev/bus/usb:/dev/bus/usb \
  custom-event-camera-image \
  /bin/bash
```

## start docker and save in the computer and mounting /dev/bus/usb as a volume
```bash
sudo docker run -it --rm \
  --name test2 \
  --net=host \
  --env DISPLAY=$DISPLAY \
  --volume "$HOME/.Xauthority:/root/.Xauthority:rw" \
  --volume "$HOME/prophesee_recording:/prophesee_recording" \
  --privileged \
  --volume /dev/bus/usb:/dev/bus/usb \
  custom-event-camera-image \
  /bin/bash
```

## add build
```bash
sudo docker run -it --rm \
  --name test2 \
  --net=host \
  --env DISPLAY=$DISPLAY \
  --volume "$HOME/.Xauthority:/root/.Xauthority:rw" \
  --volume "$HOME/prophesee_recording:/prophesee_recording" \
  --volume "$HOME/prophesee_build:/usr/share/prophesee_driver/samples/prophesee_viewer/build" \
  --privileged \
  --volume /dev/bus/usb:/dev/bus/usb \
  custom-event-camera-image \
  /bin/bash
```
