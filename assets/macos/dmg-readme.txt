Opening OpenSCP for the first time
==================================

1. Drag OpenSCP onto the Applications folder in this window.

2. Open your Applications folder and double-click OpenSCP. macOS will
   refuse to open it and report that it cannot check the app for
   malicious software. This is expected -- see "Why this happens" below.

3. Open System Settings, go to Privacy & Security, and scroll down to
   the Security section. There is a message about OpenSCP with an
   "Open Anyway" button next to it. Click it.

4. Confirm when macOS asks a second time. OpenSCP starts normally from
   then on, including after a restart.

On macOS 12 and 13 the same setting lives in System Preferences, under
Security & Privacy, in the General tab.

If you prefer the terminal, this does the same thing in one step:

    xattr -dr com.apple.quarantine /Applications/OpenSCP.app


Why this happens
----------------

macOS only skips this prompt for apps signed with a paid Apple Developer
ID. OpenSCP is free software and is not enrolled in that program, so
every build gets flagged the same way regardless of what it contains.

Every release publishes a SHA-256 checksum so you can confirm your
download matches the file that was built:

    shasum -a 256 ~/Downloads/OpenSCP-*.dmg

Compare the output against SHA256SUMS.txt on the release page.


About saved passwords
---------------------

OpenSCP keeps saved passwords in the macOS keychain. Because the app has
no stable signing identity, macOS treats each new version as a separate
application and asks for keychain permission again after an update.
Choosing "Always Allow" applies to that version only.


OpenSCP is free software under GPL-3.0-only.
https://github.com/luiscuellar31/openscp
