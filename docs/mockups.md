# Design mockups

These are the design mockups made before and during the build of first-run setup, rendered one state per picture. They are
**designs, not photos of the device**: they show what was agreed, and where the finished firmware differs the note under the
picture says so. The Wi-Fi name `Ptolemaios` and the other network names are examples.

## On the Tab5 screen

Shown at half size (the screen is 720 x 1280).

<table>
<tr>
<td align="center" valign="top" width="33%"><img src="images/mockups/tab5-1-scan.png" alt="Tab5 first-run screen with a QR code" width="260"><br><b>1. Scan</b><br>Shown on first start and when setting up again. Closes by itself after 10 minutes. The QR code here is a placeholder pattern; the real screen draws a real one.</td>
<td align="center" valign="top" width="33%"><img src="images/mockups/tab5-2-phone-joined.png" alt="Tab5 screen while the phone fills in the setup page" width="260"><br><b>2. Phone connected</b><br>Shown while the phone fills in the setup page. The finished firmware lists all four items as waiting until Save is pressed, rather than ticking them off live.</td>
<td align="center" valign="top" width="33%"><img src="images/mockups/tab5-3-saved.png" alt="Tab5 setup complete screen" width="260"><br><b>3. Setup complete</b><br>Shown for a few seconds after saving, then the Tab5 restarts onto the new settings.</td>
</tr>
<tr>
<td align="center" valign="top"><img src="images/mockups/tab5-4-quick-settings.png" alt="Tab5 quick-settings panel" width="260"><br><b>Quick settings</b><br>Swipe down from the top edge. The finished panel is icon-only: a power symbol (screen off), Wi-Fi and server symbols (link status), a moon (sleep) and, in a second row, a padlock (set up again). The brightness slider and the battery line are as drawn here.</td>
<td align="center" valign="top"><img src="images/mockups/tab5-6-pin.png" alt="Tab5 PIN keypad" width="260"><br><b>PIN keypad</b><br>Asked before setting up again. The finished screen has no "5 tries" line; it shows "Wrong PIN." or a lockout countdown instead.</td>
<td></td>
</tr>
</table>

Not shown: the sleeping screen, which is simply black.

## The setup page on the phone

The page that the Tab5 serves from its own Wi-Fi network. Five steps, then a confirmation.

<table>
<tr>
<td align="center" valign="top" width="33%"><img src="images/mockups/setup-1-wifi.png" alt="Setup page step 1: choose your Wi-Fi" width="260"><br><b>Step 1: Wi-Fi</b><br>Pick a scanned network or type the name, and enter the password. 2.4 GHz only.</td>
<td align="center" valign="top" width="33%"><img src="images/mockups/setup-2-broker-password.png" alt="Setup page step 2: broker with username and password" width="260"><br><b>Step 2: broker, username and password</b><br>Port 8883.</td>
<td align="center" valign="top" width="33%"><img src="images/mockups/setup-2-broker-certificate.png" alt="Setup page step 2: broker with a device certificate" width="260"><br><b>Step 2: broker, device certificate</b><br>Port 8884. No username or password.</td>
</tr>
<tr>
<td align="center" valign="top"><img src="images/mockups/setup-3-ca-only-password-path.png" alt="Setup page step 3 with only the CA certificate card" width="260"><br><b>Step 3: certificates, password path</b><br>Only the broker CA certificate is needed.</td>
<td align="center" valign="top"><img src="images/mockups/setup-3-certificates-empty.png" alt="Setup page step 3 with three empty certificate cards" width="260"><br><b>Step 3: certificates, device certificate path</b><br>CA certificate, device certificate and device key, each chosen as a file or pasted. Next stays grey until all three are in.</td>
<td align="center" valign="top"><img src="images/mockups/setup-3-certificates-added.png" alt="Setup page step 3 with all certificates accepted" width="260"><br><b>Step 3: all accepted</b><br>The Tab5 reads each one and reports what it found.</td>
</tr>
<tr>
<td align="center" valign="top"><img src="images/mockups/setup-4-pin.png" alt="Setup page step 4: set a PIN" width="260"><br><b>Step 4: PIN</b><br>Six digits, entered twice. Asked on the Tab5 before it lets you set it up again.</td>
<td align="center" valign="top"><img src="images/mockups/setup-5-review.png" alt="Setup page step 5: check and save" width="260"><br><b>Step 5: check and save</b><br>Secrets are masked. The finished page says the Tab5 reopens setup if the new settings cannot connect; it does not hold this page open to test them, because the setup network drops when the Tab5 joins your Wi-Fi.</td>
<td align="center" valign="top"><img src="images/mockups/setup-6-done.png" alt="Setup page confirmation" width="260"><br><b>Saved</b><br>The Tab5 restarts, so this page stops responding and can be closed.</td>
</tr>
</table>

## Sleep mode: the original plan

<p align="center"><img src="images/mockups/sleep-plan-original.png" alt="Original sleep mode plan" width="420"></p>

This is the plan from before sleep mode was built, kept for the record. **It is superseded.** The measured results are in the README: Wi-Fi power save saved nothing, the CPU clock drops to 40 MHz, and the display panel stays awake so that touch can wake the screen.
