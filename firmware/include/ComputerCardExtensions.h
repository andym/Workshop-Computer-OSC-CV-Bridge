/*
 * ComputerCardExtensions.h
 *
 * Standalone extensions for ComputerCard.h providing:
 * 1. Boot-to-USB functionality (hold switch for 2 seconds)
 * 2. Kodály-inspired startup identification patterns
 *
 * Usage: Just copy this file to your include folder alongside ComputerCard.h
 *
 * Original source : https://github.com/andym/Computer-Card-Blank-Improved
 */

#ifndef COMPUTERCARD_EXTENSIONS_H
#define COMPUTERCARD_EXTENSIONS_H

#include "ComputerCard.h"
#include "pico/bootrom.h"

namespace CardExtensions {

// =============================================================================
// SOLFÈGE NOTE PATTERNS - LED positions mimic Kodály hand signs
// =============================================================================

/**
 * Solfège note patterns - LED positions mimic Kodály hand signs
 * LED Layout: 0,1 (top row), 2,3 (middle row), 4,5 (bottom row)
 */
struct SolfegeNotes {
    static constexpr uint8_t Do[6]    = {0,0,0,0,1,1}; // Bottom only - stable, grounded (closed fist)
    static constexpr uint8_t Re[6]    = {0,0,1,1,1,1}; // Bottom + middle - moving upward
    static constexpr uint8_t Mi[6]    = {0,0,1,1,0,0}; // Middle only - higher position (flat hand)
    static constexpr uint8_t Fa[6]    = {1,0,1,1,0,0}; // Middle + one top - pointing upward (thumb down)
    static constexpr uint8_t Sol[6]   = {1,1,1,1,0,0}; // Middle + both top - broader, stronger
    static constexpr uint8_t La[6]    = {1,1,0,0,0,0}; // Top only - high position (curved fingers)
    static constexpr uint8_t Ti[6]    = {1,0,1,0,0,0}; // Top + one middle - tension (pointing finger)
    static constexpr uint8_t DoHigh[6] = {1,1,1,1,1,1}; // All LEDs - complete resolution (higher octave)
    static constexpr uint8_t Rest[6]  = {0,0,0,0,0,0}; // Silence
};

/**
 * Pre-defined startup patterns for different card types
 * Each pattern is 3 seconds long (6 beats at 120 BPM)
 */
class StartupPatterns {
public:
    // Pattern timing: note durations in half-beats (12000 samples each)
    struct Pattern {
        const uint8_t* notes[12];  // 12 half-beats = 6 beats = 3 seconds
        const char* name;
        const char* description;
    };

    // Blank/Foundation cards: "Do-Do-Do" - Simple, stable, foundational
    static const Pattern BlankCard;

    // MIDI cards: "Do-Mi-Sol" - Major triad, complete and stable
    static const Pattern MidiCard;

    // Sequencer cards: "Do-Re-Mi" - Ascending, progressive
    static const Pattern SequencerCard;

    // Effect cards: "Sol-Fa-Mi" - Descending, transformative
    static const Pattern EffectCard;

    // Utility cards: "Mi-Sol-Do'" - Upward resolution
    static const Pattern UtilityCard;

    // Sampler cards: "Do-Sol-Do" - Stable foundation with emphasis
    static const Pattern SamplerCard;

    // Rhythm cards: "Ti-Do-Do" with syncopated timing
    static const Pattern RhythmCard;

    // Experimental cards: "Fa-Ti-Re" - Unusual intervals, exploration
    static const Pattern ExperimentalCard;

    // Performance cards: "Do-Sol-Mi-Do'" - Triumphant progression
    static const Pattern PerformanceCard;

    // Developer/test cards: Scale run "Do-Re-Mi-Fa-Sol-La-Ti-Do'"
    static const Pattern DeveloperCard;
};

// =============================================================================
// EXTENDED CARD BASE CLASS
// =============================================================================

/**
 * Complete card base that handles both boot sequence and startup pattern
 * Inherit from this instead of ComputerCard directly
 *
 * This class uses the inheritance pattern to work around protected access restrictions
 */
class ExtendedCard : public ComputerCard {
private:
    // Boot management
    int switchDownCount = 0;

    // Startup pattern management
    static constexpr int SAMPLES_PER_HALF_BEAT = 12000; // 0.25s at 48kHz (120 BPM)
    const StartupPatterns::Pattern* pattern = nullptr;
    int position = 0;
    int sample_counter = 0;
    bool initialization_complete = false;

    /**
     * Handle boot sequence - returns true if should enter bootloader
     */
    bool HandleBootSequence() {
        switchDownCount = (SwitchVal() == Down) ? switchDownCount + 1 : 0;

        if (switchDownCount > 0) {
            // Clear all LEDs first
            for (int i = 0; i < 6; i++) {
                LedOff(i);
            }

            // Show progress on left column
            LedOn(4, switchDownCount > 0);      // Bottom left LED immediately
            LedOn(2, switchDownCount > 32000);  // Middle left LED at ~0.67s
            LedOn(0, switchDownCount > 64000);  // Top left LED at ~1.33s

            if (switchDownCount >= 96000) {
                Abort();
                return true;
            }
        }
        return false;
    }

    /**
     * Handle startup pattern - returns true when complete
     */
    bool HandleStartupPattern() {
        if (!pattern || initialization_complete) return true;

        sample_counter++;
        if (sample_counter >= SAMPLES_PER_HALF_BEAT) {
            sample_counter = 0;
            position++;
            if (position >= 12) {
                initialization_complete = true;
                // Clear all LEDs when pattern completes
                for (int i = 0; i < 6; i++) {
                    LedOff(i);
                }
                return true;
            }
        }

        // Display current pattern step
        if (position < 12) {
            // Clear all LEDs first
            for (int i = 0; i < 6; i++) {
                LedOff(i);
            }

            // Set LEDs according to current note pattern
            const uint8_t* current_note = pattern->notes[position];
            for (int i = 0; i < 6; i++) {
                if (current_note[i]) {
                    LedOn(i);
                }
            }
        }

        return false;
    }

protected:
    /**
     * Override this to define your card's startup pattern
     */
    virtual const StartupPatterns::Pattern& GetStartupPattern() = 0;

    /**
     * Override this for your main audio/LED processing
     * Only called after startup pattern completes
     */
    virtual void ProcessMainSample() = 0;

    /**
     * Optional: Called once when startup pattern completes
     */
    virtual void OnStartupComplete() {}

public:
    ExtendedCard() = default;

    /**
     * Main processing loop - handles boot, startup, then your code
     */
    void ProcessSample() override final {
        // Initialize startup pattern on first call (after derived constructor completes)
        if (!pattern) {
            pattern = &GetStartupPattern();
        }

        // Handle boot sequence (takes priority over everything)
        if (HandleBootSequence()) {
            return; // Card is shutting down for USB boot
        }

        // Don't run startup pattern if switch is being held
        if (switchDownCount > 0) {
            return;
        }

        // Handle startup pattern
        if (!initialization_complete) {
            if (HandleStartupPattern()) {
                OnStartupComplete();
            }
            return;
        }

        // Run main card functionality
        ProcessMainSample();
    }

    /**
     * Utility to run the card with USB boot functionality
     */
    void RunWithBootSupport() {
        Run(); // This will eventually call Abort() if switch held

        // If we get here, switch was held for 2 seconds
        // Enter USB bootloader mode with pin 11 (top right LED) as USB activity indicator
        rom_reset_usb_boot(1<<11, 0);
    }

    bool IsInitializationComplete() const { return initialization_complete; }
    bool IsSwitchHeld() const { return switchDownCount > 0; }
    const char* GetPatternName() const { return pattern ? pattern->name : "None"; }
    const char* GetPatternDescription() const { return pattern ? pattern->description : ""; }
};

// =============================================================================
// PATTERN DEFINITIONS
// =============================================================================

// Pattern implementations
const StartupPatterns::Pattern StartupPatterns::BlankCard = {
    {   // "Do-Do-Do" pattern: do(1), rest(0.5), do(1), rest(0.5), do(1), rest(1)
        SolfegeNotes::Do, SolfegeNotes::Do, SolfegeNotes::Rest,      // do(1), rest(0.5)
        SolfegeNotes::Do, SolfegeNotes::Do, SolfegeNotes::Rest,      // do(1), rest(0.5)
        SolfegeNotes::Do, SolfegeNotes::Do, SolfegeNotes::Rest,      // do(1), rest(1)
        SolfegeNotes::Rest, SolfegeNotes::Rest, SolfegeNotes::Rest
    },
    "Do-Do-Do",
    "Blank/Foundation card - Simple, stable, foundational"
};

const StartupPatterns::Pattern StartupPatterns::MidiCard = {
    {   // "Do-Mi-Sol" pattern: do(1), rest(0.5), mi(1), rest(0.5), sol(1), rest(1)
        SolfegeNotes::Do, SolfegeNotes::Do, SolfegeNotes::Rest,      // do(1), rest(0.5)
        SolfegeNotes::Mi, SolfegeNotes::Mi, SolfegeNotes::Rest,      // mi(1), rest(0.5)
        SolfegeNotes::Sol, SolfegeNotes::Sol, SolfegeNotes::Rest,    // sol(1), rest(1)
        SolfegeNotes::Rest, SolfegeNotes::Rest, SolfegeNotes::Rest
    },
    "Do-Mi-Sol",
    "MIDI card - Major triad, complete and stable"
};

const StartupPatterns::Pattern StartupPatterns::SequencerCard = {
    {   // "Do-Re-Mi" pattern: do(1), rest(0.5), re(1), rest(0.5), mi(1), rest(1)
        SolfegeNotes::Do, SolfegeNotes::Do, SolfegeNotes::Rest,      // do(1), rest(0.5)
        SolfegeNotes::Re, SolfegeNotes::Re, SolfegeNotes::Rest,      // re(1), rest(0.5)
        SolfegeNotes::Mi, SolfegeNotes::Mi, SolfegeNotes::Rest,      // mi(1), rest(1)
        SolfegeNotes::Rest, SolfegeNotes::Rest, SolfegeNotes::Rest
    },
    "Do-Re-Mi",
    "Sequencer card - Ascending, progressive"
};

const StartupPatterns::Pattern StartupPatterns::EffectCard = {
    {   // "Sol-Fa-Mi" pattern: sol(1), rest(0.5), fa(1), rest(0.5), mi(1), rest(1)
        SolfegeNotes::Sol, SolfegeNotes::Sol, SolfegeNotes::Rest,    // sol(1), rest(0.5)
        SolfegeNotes::Fa, SolfegeNotes::Fa, SolfegeNotes::Rest,      // fa(1), rest(0.5)
        SolfegeNotes::Mi, SolfegeNotes::Mi, SolfegeNotes::Rest,      // mi(1), rest(1)
        SolfegeNotes::Rest, SolfegeNotes::Rest, SolfegeNotes::Rest
    },
    "Sol-Fa-Mi",
    "Effect card - Descending, transformative"
};

const StartupPatterns::Pattern StartupPatterns::UtilityCard = {
    {   // "Mi-Sol-Do'" pattern: mi(1), rest(0.5), sol(1), rest(0.5), do'(1), rest(1)
        SolfegeNotes::Mi, SolfegeNotes::Mi, SolfegeNotes::Rest,      // mi(1), rest(0.5)
        SolfegeNotes::Sol, SolfegeNotes::Sol, SolfegeNotes::Rest,    // sol(1), rest(0.5)
        SolfegeNotes::DoHigh, SolfegeNotes::DoHigh, SolfegeNotes::Rest, // do'(1), rest(1)
        SolfegeNotes::Rest, SolfegeNotes::Rest, SolfegeNotes::Rest
    },
    "Mi-Sol-Do'",
    "Utility card - Upward resolution"
};

const StartupPatterns::Pattern StartupPatterns::SamplerCard = {
    {   // "Do-Sol-Do" pattern: do(1), rest(0.5), sol(1), rest(0.5), do(1), rest(1)
        SolfegeNotes::Do, SolfegeNotes::Do, SolfegeNotes::Rest,      // do(1), rest(0.5)
        SolfegeNotes::Sol, SolfegeNotes::Sol, SolfegeNotes::Rest,    // sol(1), rest(0.5)
        SolfegeNotes::Do, SolfegeNotes::Do, SolfegeNotes::Rest,      // do(1), rest(1)
        SolfegeNotes::Rest, SolfegeNotes::Rest, SolfegeNotes::Rest
    },
    "Do-Sol-Do",
    "Sampler card - Stable foundation with emphasis"
};

const StartupPatterns::Pattern StartupPatterns::RhythmCard = {
    {   // "Ti-Do-Do" with syncopation: ti(0.5), do(1), do(1), rest(0.5), ti(0.5), rest(1.5)
        SolfegeNotes::Ti, SolfegeNotes::Do, SolfegeNotes::Do,        // ti(0.5), do(1)
        SolfegeNotes::Do, SolfegeNotes::Do, SolfegeNotes::Rest,      // do(1), rest(0.5)
        SolfegeNotes::Ti, SolfegeNotes::Rest, SolfegeNotes::Rest,    // ti(0.5), rest(1.5)
        SolfegeNotes::Rest, SolfegeNotes::Rest, SolfegeNotes::Rest
    },
    "Ti-Do-Do",
    "Rhythm card - Syncopated timing"
};

const StartupPatterns::Pattern StartupPatterns::ExperimentalCard = {
    {   // "Fa-Ti-Re" pattern: fa(1), rest(0.5), ti(1), rest(0.5), re(1), rest(1)
        SolfegeNotes::Fa, SolfegeNotes::Fa, SolfegeNotes::Rest,      // fa(1), rest(0.5)
        SolfegeNotes::Ti, SolfegeNotes::Ti, SolfegeNotes::Rest,      // ti(1), rest(0.5)
        SolfegeNotes::Re, SolfegeNotes::Re, SolfegeNotes::Rest,      // re(1), rest(1)
        SolfegeNotes::Rest, SolfegeNotes::Rest, SolfegeNotes::Rest
    },
    "Fa-Ti-Re",
    "Experimental card - Unusual intervals, exploration"
};

const StartupPatterns::Pattern StartupPatterns::PerformanceCard = {
    {   // "Do-Sol-Mi-Do'" triumphant: do(0.5), sol(0.5), mi(0.5), do'(1.5), rest(1), do'(1)
        SolfegeNotes::Do, SolfegeNotes::Sol, SolfegeNotes::Mi,       // do(0.5), sol(0.5), mi(0.5)
        SolfegeNotes::DoHigh, SolfegeNotes::DoHigh, SolfegeNotes::DoHigh, // do'(1.5)
        SolfegeNotes::Rest, SolfegeNotes::Rest, SolfegeNotes::DoHigh, // rest(1), do'(1)
        SolfegeNotes::DoHigh, SolfegeNotes::Rest, SolfegeNotes::Rest
    },
    "Do-Sol-Mi-Do'",
    "Performance card - Triumphant progression"
};

const StartupPatterns::Pattern StartupPatterns::DeveloperCard = {
    {   // Complete scale: do(0.5), re(0.5), mi(0.5), fa(0.5), sol(0.5), la(0.5), ti(0.5), do'(0.5)
        SolfegeNotes::Do, SolfegeNotes::Re, SolfegeNotes::Mi,        // do, re, mi
        SolfegeNotes::Fa, SolfegeNotes::Sol, SolfegeNotes::La,       // fa, sol, la
        SolfegeNotes::Ti, SolfegeNotes::DoHigh, SolfegeNotes::Rest,  // ti, do', rest
        SolfegeNotes::Rest, SolfegeNotes::Rest, SolfegeNotes::Rest
    },
    "Scale Run",
    "Developer card - Complete scale for testing"
};

} // namespace CardExtensions

#endif // COMPUTERCARD_EXTENSIONS_H