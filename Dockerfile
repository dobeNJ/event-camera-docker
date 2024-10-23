# Use Ubuntu 18.04 as the base image
FROM ubuntu:18.04

# Set environment variable to avoid dialog prompts during installation
ENV DEBIAN_FRONTEND=noninteractive

# Install necessary dependencies (X11 support, certificates, wget, etc.)
RUN apt-get update && apt-get install -y \
    wget \
    curl \
    ca-certificates \
    gnupg2 \
    software-properties-common \
    libopencv-dev \
    libboost-program-options-dev \
    x11-apps

# Set up authentication for Prophesee repository
RUN mkdir -p /etc/apt/auth.conf.d
RUN echo 'machine apt.prophesee.ai login prophesee password DbnLdKL5YXnMndWg' > /etc/apt/auth.conf.d/prophesee-auth.conf

# Add Prophesee repository for Ubuntu 18.04
RUN echo 'deb [arch=amd64 trusted=yes] https://apt.prophesee.ai/dists/ubuntu bionic main' >> /etc/apt/sources.list

# Ensure proper permissions for the sources.list file
RUN chmod 644 /etc/apt/sources.list

# Install Metavision SDK and camera dependencies
RUN apt-get update && apt-get install -y \
    prophesee-* \
    metavision-*

# Set environment variable for X11 display (adjust as needed)
ENV DISPLAY=:0

# Expose port if necessary (for hardware or communication purposes)
EXPOSE 5000

# Set the CMD to run the Metavision Vibration Monitoring demo with the camera
CMD ["metavision_vibration_monitoring_demo"]
