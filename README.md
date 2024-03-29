# Play MP3 stream from AWS Polly

This example is an expanded version of [Play MP3 stream from AWS Polly](https://github.com/espressif/esp-adf/tree/master/examples/cloud_services/pipeline_aws_polly_mp3)
to include a web page where the text to be played and the voice to be used can be entered.
The goal of this example is to show how to use Audio Pipeline to play audio generated from text by [Amazon Polly](https://aws.amazon.com/polly/) service.


## Compatibility

| ESP32-LyraT | ESP32-LyraT-MSC |
|:-----------:|:---------------:|
| [![alt text](images/esp32-lyrat-v4.2-side-small.jpg "ESP32-LyraT")](https://docs.espressif.com/projects/esp-adf/en/latest/get-started/get-started-esp32-lyrat.html) | [![alt text](images/esp32-lyratd-msc-v2.2-small.jpg "ESP32-LyraTD-MSC")](https://docs.espressif.com/projects/esp-adf/en/latest/get-started/get-started-esp32-lyratd-msc.html) |
| ![alt text](images/yes-button.png "Compatible") | ![alt text](images/yes-button.png "Compatible") |

## Usage

Prepare the audio board:

- Connect speakers or headphones to the board. 

Configure the example:

- Select compatible audio board in `menuconfig` > `Audio HAL`
- Set up the Wi-Fi connection by running `menuconfig` > `Example Configuration` and filling in `WiFi SSID` and `WiFi Password`.
- Under the same menu provide `AWS_ACCESS_KEY` and `AWS_SECRET_KEY`

Prepare the SD card.
- create a folder named 'www'.
- copy the files in the 'www' folder to the SD card's 'www' folder.

Load and run the example.

For more details on how to use the Amazon Polly service you can refer to [Getting Started with Amazon Polly](https://docs.aws.amazon.com/polly/latest/dg/getting-started.html)
