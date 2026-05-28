# 3DSync

Homebrew for Nintendo 3DS/2DS console family that allows synchronization of saves and files to a cloud, to another console or to a PC

Configurator has built-in support for Checkpoint and JKSM folders, but you can add custom one if you like

**currently in early stage, supports Dropbox and Google Drive to upload files**

Full credit goes to [Kyraminol](github.com/Kyraminol). As his project was abandoned, I picked it up and extended it with Google Drive Support.

## Usage

1. Follow steps on the [configurator page](https://vllni.github.io/3DSync/) to obtain the configuration file
2. Place the configurator file in the following folder of the console SD card: `/3ds/3DSync/3DSync.ini`
3. Download and install .cia file **or** run the .3dsx from the homebrew launcher

## Cloud providers

Dropbox uses the `[Dropbox]` section with a `token` value.

Google Drive uses the `[GoogleDrive]` section with a `token` value and an optional `folderId` value. The Google Drive token must have permission to create files in Drive. Uploaded Google Drive filenames include the configured path name and relative file path with `/` replaced by `_`.
