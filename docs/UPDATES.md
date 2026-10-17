# Updating OpenSCP

Use **OpenSCP → Check for updates…** to check for a newer stable release.
**Automatically check for updates** enables a daily check and is off by default.
Checks use HTTPS and do not send saved-site information or credentials.
Installing an update requires your confirmation and waits for pending file
operations to finish.

## Choose your installation method

| Installation | How to update |
| --- | --- |
| macOS app | Builds with in-app updates offer installation and relaunch. Otherwise, download the newer app from the releases page. Run the app from Applications, not from the DMG. |
| Linux AppImage | Builds with in-app updates verify the download and replace the image after confirmation. Keep the AppImage in a folder owned by you where you can write. Restart OpenSCP after installation. |
| Flatpak from a repository | Use your software manager or `flatpak update io.github.luiscuellar31.openscp`. |
| Standalone Flatpak bundle | Download and install the newer bundle if no update repository is configured. |
| Snap from Snap Store | snapd updates it automatically; `sudo snap refresh openscp` requests an update. |
| Other or source builds | Use the releases page or your original installation method. |

If in-app installation is unavailable, use the
[releases page](https://github.com/luiscuellar31/openscp/releases)
or the package manager you installed OpenSCP with. A sideloaded Snap does not
receive Snap Store updates automatically.

The first version with an updater must be installed manually: v1.1.0 does not
include an updater.

## AppImage backup

After an in-app update, the previous AppImage remains beside the current one as
`<filename>.previous`. To restore it, close OpenSCP and replace the current image
with that backup using the original filename.
