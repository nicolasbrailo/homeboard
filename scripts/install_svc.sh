sudo cp ./ambience.service /etc/systemd/system/ambience.service
sudo systemctl daemon-reload
sudo systemctl enable ambience
sudo systemctl start ambience
sudo systemctl status ambience

