#!/bin/bash

# הגדרת שם הממשק
TAP_DEV="tap0"

echo "Setting up TAP device: $TAP_DEV"

# 1. יצירת הממשק
sudo ip tuntap add mode tap $TAP_DEV

# 2. הגדרת ה-IP של המחשב שלך (הצד של ה-Host)
sudo ip addr add 10.0.0.1/24 dev $TAP_DEV

# 3. הפעלת הממשק
sudo ip link set dev $TAP_DEV up

echo "TAP device is ready. Host IP: 10.0.0.1"
echo "The TCP Stack will use: 10.0.0.2"