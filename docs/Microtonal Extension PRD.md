> **Status: NOT STARTED** — No microtonal parsing or tuning engine.

# **Product Requirement Document: Akkado Microtonal Extension**

**Version:** 1.0

**Status:** Draft / Planned

**Feature:** Microtonal Notation & Tuning Engine

## **1\. Objective**

To extend the Akkado mini-notation syntax to support arbitrary, agnostic microtonal composition. The system must allow users to express precise pitch deviations (using steps, cents, or ratios) independent of the underlying tuning system (12-EDO, 31-EDO, JI, etc.), preserving the fluidity of live coding.

## **2\. Core Philosophy**

* **Agnostic Notation:** The syntax defines *relative relationships* (e.g., "up one step"), not absolute frequencies.  
* **Decoupled Interpretation:** A tune() function acts as the interpreter, translating notation artifacts into Hertz based on the active tuning context.  
* **Backward Compatibility:** Standard Western notation (c\#4, Bb5) must function exactly as before when no custom tuning is applied.

## **3\. Syntax Specification**

### **3.1. Extended Pitch Atom**

The Pitch Atom definition is expanded to accept a **stream of modifiers** between the Note Nominal and the Octave.

**Format:** \[Note Nominal\] \+ \[Modifier Stream\] \+ \[Octave\]

### **3.2. Microtonal Operators**

Two new primary operators are introduced to control micro-steps.

| Operator | Name | Function |
| :---- | :---- | :---- |
| ^ | Step Up | Increments the micro\_offset counter by 1\. |
| v | Step Down | Decrements the micro\_offset counter by 1\. |

### **3.3. Numeric Arguments**

To support large intervals or specific harmonic indices, operators accept numeric arguments.

* ^n (e.g., ^4) is semantically equivalent to repeating the operator n times (^^^^).  
* vn (e.g., v2) is semantically equivalent to repeating v n times.

## **4\. Parser Implementation Requirements**

### **4.1. The "Stacking" Strategy**

The parser must not rely on rigid token matching (e.g., looking for "bb" or "x"). Instead, it must parse the modifier section as a continuous stream of characters, accumulating values into two distinct registers.

**Registers:**

1. **accidental\_std (Standard):** Tracks chromatic semitones (12-TET basis).  
2. **micro\_offset (Micro):** Tracks system-specific micro-steps.

**Accumulation Logic Table:**

| Character | Register Affected | Delta | Description |
| :---- | :---- | :---- | :---- |
| \# | accidental\_std | \+1 | Standard Sharp |
| b | accidental\_std | \-1 | Standard Flat |
| ^ | micro\_offset | \+1 | Micro Step Up |
| v | micro\_offset | \-1 | Micro Step Down |

### **4.2. Alias Support**

To support community standards (Stein-Zimmermann, HEJI), the parser must recognize specific ASCII aliases. These aliases map directly to the registers above.

| Alias | Register | Delta | Standard Meaning |
| :---- | :---- | :---- | :---- |
| d | micro\_offset | \-1 | Inverted Flat / Quarter-tone Flat |
| \\ | micro\_offset | \-1 | Alternative Down |
| \+ | micro\_offset | \+1 | Half Sharp / Quarter-tone Sharp |
| x | accidental\_std | \+2 | Double Sharp |

**Parsing Edge Case:**

The character d is ambiguous (Note D vs. Alias d).

* **Rule:** If d appears immediately following a Note Nominal or another Modifier (e.g., cd4 or cbd4), it is parsed as an Alias.  
* **Rule:** If d is separated by whitespace (e.g., c d4), it is parsed as a new Pitch Atom.

### **4.3. Parsing Examples & Resulting State**

| Input String | accidental\_std | micro\_offset | Rationale |
| :---- | :---- | :---- | :---- |
| c\#4 | \+1 | 0 | Standard sharp |
| c^4 | 0 | \+1 | Pure micro step |
| c\#^4 | \+1 | \+1 | Sharp \+ Micro step (Mixed) |
| cbb4 | \-2 | 0 | Stacked flats |
| cbd4 | \-1 | \-1 | Flat \+ Inverted Flat (Sesquiflat) |
| c+4 | 0 | \+1 | Alias usage |

## **5\. Architecture & Data Flow**

### **5.1. Event Object Structure**

The parser yields an event object that separates the nominal, standard chromatic shifts, and microtonal shifts.

interface PitchEvent {  
  note: number;          // MIDI number of the Nominal (e.g., C4 \= 60\)  
  accidental\_std: number;// Accumulated standard modifiers (semitones)  
  micro\_offset: number;  // Accumulated micro modifiers (steps)  
  // ... velocity, duration, etc.  
}

### **5.2. The Tuning Pipe (tune())**

The tune() function transforms the PitchEvent into a frequency (freq). It serves as the bridge between the agnostic notation and the specific tuning system.

**Signature:**

tune(systemId: string, options?: TuningOptions): PatternFunction

**Calculation Logic (Pseudocode):**

* **Scenario A: EDO (Equal Division of Octave)**  
  * *Constraint:* accidental\_std usually maps to multiple EDO steps (e.g., flat \= 2 steps in 31-EDO), while micro\_offset maps to 1 step.  
  * StepSize \= 1200 / EDO\_Count  
  * Nominal\_Cents \= MidiToCents(event.note)  
  * Std\_Shift \= event.accidental\_std \* (StepSize \* Semitone\_Multiplier)  
  * Micro\_Shift \= event.micro\_offset \* StepSize  
  * Total\_Cents \= Nominal\_Cents \+ Std\_Shift \+ Micro\_Shift  
* **Scenario B: Just Intonation**  
  * The micro\_offset acts as an index traverser in a ratio array, relative to the nominal anchor.

## **6\. Standard Library & Configuration**

### **6.1. Built-in Library**

The system must ship with definitions for common xenharmonic systems.

* **12edo** (Default)  
* **17edo, 19edo, 22edo** (Superparticular)  
* **24edo** (Quarter-tone)  
* **31edo** (Meantone/Huygens)  
* **41edo, 53edo** (High-count approximations)  
* **ji** (5-limit symmetric)  
* **bp** (Bohlen-Pierce non-octave)

### 

### **6.2. External Loading API**

Users must be able to load .scl (Scala) and .tun files.

await load\_tuning("slendro", "assets/slendro.scl");  
pat("n0 n1 n2") |\> tune("slendro")

## **7\. Development Roadmap**

1. **Phase 1: Parser Update**  
   * Refactor Pitch Atom regex/logic to support "Modifier Stream" parsing.  
   * Implement accidental\_std vs micro\_offset split in the Event Object.  
   * Add unit tests for stacking (bb, bd, \#^).  
2. **Phase 2: Tuning Engine**  
   * Implement tune() pipe.  
   * Create EDO class logic.  
3. **Phase 3: Library & Aliases**  
   * Implement the Preset/Alias map.  
   * Populate the standard library.