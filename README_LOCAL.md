```
# link usb
sudo usbip attach -r $(ip route | grep default | awk '{print $3}') -b 2-1
```