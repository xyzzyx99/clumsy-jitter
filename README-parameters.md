# Clumsy-jitter: File > Load/Save parameters

This version adds a **File** menu:

- **Load parameters ...** selects a `.ini` parameter file and updates the GUI and its active module values.
- **Save parameters ...** saves the current GUI configuration in a `.ini` file.
- **Exit** stops packet interception (if running) and exits.

The saved file contains the current filter expression, the selected preset index (0 = custom), the enabled/disabled state of all eight modules, every Inbound/Outbound and special checkbox (for example Drop Throttled and Redo Checksum), and all numeric fields (Delay, Jitter, Chance, Timeframe, Count, and Bandwidth Limit).

Parameter files are separate from the existing `config.txt`, which defines the **Presets** dropdown; loading a parameter file does not replace or edit those definitions. The Save operation refuses the filename `config.txt` to avoid overwriting a presets file.

### Automatic last-used settings (`.clumsy.ini`)

On a normal GUI exit (File > Exit, window close, or an application timeout), Clumsy silently saves the current filter, selected preset, all checkbox states, and numeric fields to `.clumsy.ini`. When the application opens again, it silently restores those settings **without starting packet interception**. No overwrite prompts or error dialogs are displayed.

- **Saving:** Try `.clumsy.ini` next to `clumsy.exe`. If writing fails (including a read-only target file), try `%USERPROFILE%\.clumsy.ini`. If both fail, do nothing.
- **Loading:** Try the file next to `clumsy.exe` first. If it is missing, unreadable, or invalid, try `%USERPROFILE%\.clumsy.ini`. If neither file can be loaded, retain the usual defaults without warning.
- **Precedence:** A valid `.clumsy.ini` next to the executable always takes priority over the copy in the home directory, even if the home copy is newer.
- **CLI:** Command-line-driven sessions are not automatically loaded or saved, so scripted overrides do not replace GUI preferences. A failed/duplicate-instance launch also does not overwrite the preferences.
- **Manual File menu:** The existing Load parameters ... and Save parameters ... commands still use a file picker and preserve their original error messages. They never automatically overwrite `.clumsy.ini` unless the user explicitly selects it.
- **Network safety:** `.clumsy.ini` restores values only. Interception still starts manually, and no WinDivert driver change is necessary.

### When changes take effect

- Checkbox and numeric values take effect immediately for newly processed packets, just like changes made manually in the GUI.
- A loaded filter expression takes effect on **Start** (if stopped), or after clicking **Re-apply** (if running). Loading does **not** automatically restart filtering and does not reset the watchdog.
- Saving does not change the running session.
- Changing values does not retroactively reschedule already queued delayed packets.

### File format

The `.ini` file is plain text with stable named keys, for example:

```ini
# Clumsy-jitter GUI parameters (not filter presets)
format=clumsy-jitter-parameters-v1
filter=outbound and tcp
preset=0
lag.enabled=1
lag.inbound=1
lag.outbound=1
lag.delay=100
lag.jitter=20
```

Special characters in values are percent-escaped. The loader checks the format, requires all settings, rejects duplicate and invalid values, and makes no changes if any validation fails. It ignores unknown keys to permit optional future additions. Files from different builds with different sets of controls may need conversion.

### Build and compatibility

- GENie Visual Studio builds automatically include the new `src/parameters.c` file through `src/**.c`.
- The Zig source list in `build.zig` has also been updated.
- Existing WinDivert watchdog behavior, Re-apply button, original presets, and optimized compiler options are unchanged.
- The separate WinDivert driver does **not** need to be rebuilt for this GUI feature.

No Windows compilation or interactive GUI test was available in the authoring environment; build and test on your Windows runner before deploying.
