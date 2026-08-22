# Phase 7 — Device UI System

## Objective

Define and implement the minimal Onda device UI for the 1.54" four-colour e-Paper display.

The UI should communicate device state clearly while minimising e-Paper refreshes.

The display must be treated as a slow, persistent surface rather than a continuously updating screen.

Simplicity and low refresh frequency are primary requirements.

## Accepted Implementation Boundaries

- Add `components/device_ui` between application state and display transport.
  Application code supplies declarative primary, Wi-Fi, and battery status; it
  does not compose display layouts.
- Production battery status is `UNKNOWN`. This phase renders all coarse levels
  but does not add ADC measurement or power logic; Phase 011 supplies values.
- Current audio capture is not persisted, so stopping recording returns to
  `READY`. Do not render `SAVED` until storage verifies a recording.
- Do not add a Booting screen, live timers, a preview/playground build, or a
  controlled-failure build. Hardware validation uses normal audio/Wi-Fi flows.

## Design Principles

The device UI should feel:

- minimal
- calm
- obvious
- product-focused
- readable at a glance

Avoid unnecessary visual elements.

Every display refresh should have a clear product reason.

The UI should communicate state, not activity animation.

## Display Constraints

Target display:

- 200 × 200
- black
- white
- red
- yellow
- slow full refresh
- visible flashing during refresh

Because refreshes are visually disruptive:

- do not animate
- do not update timers every second
- do not render progress animations
- do not continuously update signal strength
- do not refresh for minor state changes
- avoid periodic redraws

Prefer static screens that remain visible until the next meaningful device state transition.

## Layout

Use a consistent layout across all primary Onda screens.

The display should have a persistent top status bar.

Conceptually:

    ┌────────────────────────┐
    │ ONDA          WiFi Batt│
    │                        │
    │                        │
    │        Ready           │
    │                        │
    │                        │
    │                        │
    └────────────────────────┘

### Top Bar

The top bar is persistent across primary device screens.

Left side:

- `ONDA` wordmark
- always visible
- consistent position and size

Right side:

- Wi-Fi status icon
- battery status icon

Keep status icons small and visually secondary to the main device state.

### Main Content

The centre of the display communicates the primary device state.

Examples:

    Ready

    Recording

    Wi-Fi setup

    Error

The primary state should be the strongest element in the visual hierarchy.

### Secondary Content

The lower area may contain short contextual information when required.

Examples:

    Use your phone to configure

    Recording available

Avoid secondary information when it does not help the user take an action or understand the current state.

### Status Bar Refresh Rules

The status bar must follow the same minimal-refresh philosophy as the rest of the UI.

Do not refresh the display solely because:

- Wi-Fi signal strength changed
- battery percentage changed slightly
- Wi-Fi briefly disconnected or reconnected

Prefer coarse status representations.

For Wi-Fi:

    connected
    offline

For battery:

    high
    medium
    low
    critical

Update these indicators when the display is already refreshing for another meaningful state change. Until Phase 011 provides a measured level, production renders the `unknown` battery icon.

A future power feature may make a critical battery condition trigger its own
refresh if user attention is required.

Avoid periodic status-bar refreshes.

## Core UI States

Implement reusable screens for the following application states.

### Booting

Not implemented in this phase. Startup does not justify an additional transient
e-Paper refresh before the first actionable or Ready screen.

### Ready

Primary idle state.

Conceptually:

    ONDA             WiFi Batt


             Ready

This should be the normal resting screen.

### Recording

Displayed once when recording starts.

Conceptually:

    ONDA             WiFi Batt


          ● Recording

Use red sparingly to communicate active recording.

Do not display a live timer.

The screen should remain unchanged for the entire recording session.

### Saving

Not implemented in this phase because recording persistence has not been added.

### Saved

Deferred. The current recorder does not persist audio, so this state must not
be shown until a later storage feature verifies a file successfully.

### Offline

Offline should normally be represented by the status bar rather than replacing the primary application state.

For example:

    ONDA          Offline Batt


             Ready

The device should still clearly communicate that recording is available.

Avoid a dedicated `Offline` screen unless the loss of connectivity requires user attention.

### Wi-Fi Setup

Used during provisioning.

Conceptually:

    ONDA          Offline Batt


          Wi-Fi setup

       Use your phone
       to configure Onda

Reuse the existing Wi-Fi provisioning behaviour.

### Connecting

Do not render a dedicated Connecting screen. Keep the existing screen while
the Wi-Fi component connects or retries, then apply its resulting status icon
on the next meaningful UI refresh. Provisioning completion is meaningful
because it transitions from Wi-Fi setup to Ready.

### Error

Provide a simple reusable error screen.

Conceptually:

    ONDA             WiFi Batt


      Something went wrong

            Try again

Do not expose raw technical errors to the user.

Detailed errors belong in serial logs.

## Refresh Policy

Create an explicit display refresh policy.

A display refresh should occur only when entering a visually meaningful state.

Examples:

    READY → RECORDING       refresh
    RECORDING → READY       refresh
    READY → WIFI_SETUP      refresh
    WIFI_SETUP → READY      refresh
    any state → ERROR        refresh

Do not refresh for:

    retry #1 → retry #2
    audio buffer updates
    signal level changes
    small battery changes
    Wi-Fi signal-strength changes
    short network interruptions

Avoid refreshing when the next state is likely to occur within a few seconds.
Status-bar-only Wi-Fi and battery changes are retained in application state and
rendered with the next primary-state transition.

## State Coalescing

Where practical, skip transient states to reduce flashing.

For example, instead of:

    RECORDING
       ↓ refresh
    SAVING
       ↓ refresh
    READY

prefer:

    RECORDING
       ↓
    saving internally
       ↓ refresh
    READY

if saving completes quickly.

The `device_ui` refresh policy decides whether a state deserves a visible screen.

## Architecture

Keep UI state separate from hardware display implementation.

Conceptually:

    application state
          ↓
      device UI
          ↓
    display abstraction
          ↓
    e-Paper driver

Use a small declarative UI interface such as:

    device_ui_show(state)
    device_ui_should_refresh(previous, next)

The state contains primary, Wi-Fi, battery, and provisioning-PoP data. The
display component accepts only a generic fixed-layout screen descriptor.

Application logic should not draw directly to the e-Paper driver.

The UI layer should compose:

- persistent top bar
- primary state
- optional secondary content

## Colour Usage

Use white and black for most of the interface.

Use red and yellow only as semantic accents.

Suggested usage:

- red: active recording / critical error
- yellow: warning / setup attention
- black: primary text and icons
- white: background

Do not use colour merely for decoration.

State must remain understandable without relying exclusively on colour.

## Typography

Use the existing Waveshare-supported fonts initially.

Prioritise:

- readability
- clear hierarchy
- minimal number of sizes
- consistent alignment

The `ONDA` wordmark should have a stable size and position.

The primary state should be visually stronger than the top status bar.

Do not introduce custom fonts in this phase unless required.

## Icons

Implement simple static icons for:

- Wi-Fi
- battery

Keep them:

- small
- monochrome where possible
- visually consistent
- easy to understand at 200 × 200 resolution

Do not add an icon library.

Prefer small static assets or simple drawing primitives supported by the existing display layer.

### Wi-Fi Icon

The Wi-Fi icon only needs to communicate coarse connectivity.

Do not represent live signal strength.

At minimum:

    connected
    offline

### Battery Icon

The battery icon should use coarse levels rather than frequently changing percentages.

At minimum:

    high
    medium
    low
    critical

Do not display a live numeric battery percentage if doing so encourages unnecessary refreshes. The complete level set is unit-tested; physical production rendering uses `unknown` until the power feature is implemented.

## Recording UX

Recording is the most important device state.

Requirements:

- recording state must be unmistakable
- one refresh when recording starts
- no display updates while recording
- no timer updates
- no waveform
- no audio level visualisation
- recording continues independently from display behaviour

The display should consume effectively zero attention while the meeting is being recorded.

## Network UX

Wi-Fi state should be secondary to recording capability.

Do not repeatedly refresh the display because:

- Wi-Fi signal changed
- retries are occurring
- the device briefly disconnected or reconnected

Prefer showing the current network state the next time another meaningful UI refresh occurs.

Provisioning is an exception because it requires user action.

## Error Handling

UI rendering failures should:

- be logged clearly
- not crash recording or audio logic
- avoid repeated refresh loops

If the display becomes unavailable, core recording behaviour should continue where possible.

## Debug UI Mode

Do not add a preview mode or controlled-failure build in this phase. Validate
the reachable screens through normal recording and Wi-Fi flows. Error mapping
and all battery-level mappings are covered by automated unit tests and code
review instead of injected hardware faults.

## Non-goals

Do not implement:

- live recording timer
- animations
- waveforms
- menus
- settings screens
- touch interaction
- complex navigation
- transcript display
- meeting summaries
- scrolling UI
- partial-refresh optimisation
- LVGL
- custom UI framework
- live battery percentage updates
- Wi-Fi signal-strength indicators
- arbitrary notifications

## Validation

Automated:

    get_idf
    idf.py build

Build the ESP-IDF Unity tests for `device_ui` and run them on the device when
test hardware is available. They cover state-to-screen mapping, refresh
suppression, Error content, and all coarse battery states.

Physical-device validation:

1. Validate every UI state reachable through normal recording and Wi-Fi flows.
2. Confirm `ONDA` remains consistently positioned at the top left.
3. Confirm Wi-Fi and battery indicators remain consistently positioned at the top right.
4. Confirm the main state is visually dominant.
5. Confirm text and icons are readable at normal viewing distance.
6. Confirm red/yellow accents render correctly.
7. Confirm the number of refreshes is minimal.
8. Confirm recording does not trigger repeated refreshes.
9. Confirm transient internal states can be skipped where appropriate.
10. Confirm Wi-Fi retries do not cause display flashing.
11. Confirm battery changes do not cause periodic refreshes.
12. Confirm repeated state changes do not crash or corrupt the display.
13. Confirm existing button, audio, recording, and Wi-Fi functionality remains stable.
14. Confirm the `unknown` battery icon is shown until a future power feature supplies a level.

## Completion Criteria

This phase is complete when:

1. Onda has a consistent device UI layer.
2. All primary screens share the same layout.
3. `ONDA` is persistently positioned at the top left.
4. Wi-Fi and battery status are positioned at the top right.
5. The primary application state occupies the central content area.
6. Core device states have dedicated reusable screens.
7. Application code no longer manages low-level display layout directly.
8. Recording causes only one display refresh when entering the state.
9. No periodic display refresh occurs during recording.
10. Network and battery changes do not cause unnecessary refreshes.
11. Transient states are skipped where appropriate.
12. Colour use remains minimal and semantic.
13. All states reachable without fault injection can be reviewed on physical hardware; Error and non-unknown battery levels have automated mapping coverage.
14. Firmware builds and runs successfully on the physical board.
15. Existing device functionality remains stable.

## Implementation Instructions

Before implementation:

1. Read `AGENTS.md`.
2. Read `docs/development.md`.
3. Inspect the existing display abstraction.
4. Inspect the current application, recording, Wi-Fi, and battery capabilities.
5. Identify every current display refresh and why it occurs.
6. Propose the shared top-bar layout and minimal UI state model before modifying code.
7. Propose an explicit refresh policy before implementation.

Optimise for clarity and minimal refreshes.

The e-Paper should change only when the user genuinely needs new information.
