# UML 2.5.1 StateMachine Specification

## 14.2.3 Semantics

### 14.2.3.1 StateMachine

A behavior StateMachine comprises one or more **Regions**, each Region containing a graph (possibly hierarchical) comprising a set of **Vertices** interconnected by arcs representing **Transitions**.

#### Core Concepts

* **State machine execution** is triggered by appropriate Event occurrences
* A particular execution is represented by a set of valid path traversals through one or more Region graphs
* Transitions are triggered by dispatching Event occurrences that match active Triggers

#### Context

**With BehavioredClassifier context:**

* The Classifier defines which Signal and CallEvent triggers are applicable
* Features available to Behaviors owned by the StateMachine are defined by this Classifier
* Signal Triggers and CallEvent Triggers are defined according to Receptions and Operations

**Without BehavioredClassifier context (stand-alone):**

* Triggers don't need to be tied to Receptions or Operations
* Can be defined as a Template with Triggers as TemplateParameters
* Can be reused with different context Classifiers

#### Event Pool

By definition, invocations of StateMachine executions result in triggered effects, and there is an associated **event pool** with such an execution. The event pool belongs to either:

* Its context Classifier object, or
* If defining a method of a BehavioralFeature, to the instance of the Classifier owning the BehavioralFeature

#### Execution States

Due to its event-driven nature, a StateMachine execution is either:

* **In transit** - when an event is dispatched that matches at least one associated Trigger
* **In state** - alternating with transit state

> **NOTE:** A StateMachine execution may be executing Behaviors even when in a stable state configuration, in cases where there are doActivity Behaviors associated with its active state configuration.

***

### 14.2.3.2 Regions

A **Region** denotes a behavior fragment that may execute concurrently with its orthogonal Regions.

#### Orthogonality

Two or more Regions are **orthogonal** to each other if they are either:

* Owned by the same State, or
* At the topmost level, owned by the same StateMachine

#### Activation

A Region becomes active (begins executing) either when:

* Its owning State is entered, or
* If directly owned by a StateMachine (top level Region), when the StateMachine starts executing

Each Region owns:

* A set of Vertices
* A set of Transitions (determining behavioral flow)
* Its own initial Pseudostate (optional)
* Its own FinalState (optional)

#### Default Activation

Occurs if the Region is entered **implicitly**:

* Not entered through an incoming Transition terminating on one of its component Vertices
* Entered through a (local or external) Transition terminating on the containing State
* In case of top level Region, when the StateMachine starts executing

**Default activation** means execution starts with the Transition originating from the initial Pseudostate of the Region, if defined.

If no initial Pseudostate exists:

* One approach: deem the model ill defined
* Alternative: the Region remains inactive (containing composite State treated as simple/leaf State)

#### Explicit Activation

Occurs when a Region is entered by a Transition terminating on one of its contained Vertices.

When one Region of an orthogonal State is activated explicitly:

* Results in default activation of all orthogonal Regions
* Unless those Regions are also entered explicitly
* Multiple orthogonal Regions can be entered explicitly in parallel through Transitions from the same fork Pseudostate

***

### 14.2.3.3 Vertices

**Vertex** is an abstract class capturing common characteristics for different concrete kinds of nodes:

* States
* Pseudostates
* ConnectionPointReferences

#### Characteristics

* With exceptions, a Vertex can be the source and/or target of any number of Transitions
* Semantics depend on the concrete kind of node represented

#### Transitive vs. Stable Vertices

**Transitive** (Pseudostates and ConnectionPointReferences):

* Compound transition execution simply passes through them
* Arriving on incoming Transition, leaving on outgoing Transition without pause

**Stable** (State and FinalState):

* When StateMachine execution enters them, it remains until:
  * Some Event triggers a transition to a different State, or
  * The StateMachine is terminated

***

### 14.2.3.4 States

A **State** models a situation in the execution of a StateMachine Behavior during which some invariant condition holds.

In most cases this condition is not explicitly defined but implied (usually through the name).

#### 14.2.3.4.1 Kinds of States

1. **Simple State** (`isSimple = true`)
   * No internal Vertices or Transitions

2. **Composite State** (`isComposite = true`)
   * Contains at least one Region
   * Can be:
     * **Simple composite State**: exactly one Region
     * **Orthogonal State**: multiple Regions (`isOrthogonal = true`)

3. **Submachine State** (`isSubmachineState = true`)
   * Refers to an entire StateMachine
   * Conceptually "nested" within the State

#### State Terminology

* **Substate**: Any State enclosed within a Region of a composite State
* **Direct substate**: Not contained in any other State
* **Indirect substate**: Contained within another State

#### 14.2.3.4.2 State Configurations

A particular "state" of an executing StateMachine instance is represented by one or more hierarchies of States, forming a **state configuration**.

**Example configuration:**

```
<CourseAttempt - Studying – (Studying::Lab2, Studying::TermProject, Studying::FinalTest)>
```

* An executing StateMachine instance can only be in exactly one state configuration at a time
* This is referred to as its **active state configuration**
* StateMachine execution = transitions from one active state configuration to another

#### Active State

A State is **active** if it is part of the active state configuration.

#### Stable Configuration

A state configuration is **stable** when:

* No further Transitions from that configuration are enabled, AND
* All entry Behaviors have completed (but not necessarily doActivity Behaviors)

**Important:**

* After creation and initial Transition, a StateMachine is always "in" some state configuration
* Entering a hierarchical state configuration involves a dynamic process
* Terminates only after a stable state configuration is reached

A configuration is deemed stable even if there are deferred, completion, or other Event occurrences pending in the event pool.

#### 14.2.3.4.3 State Entry, Exit, and doActivity Behaviors

**Entry Behavior:**

* Associated behavior executed whenever State is entered through external Transition

**Exit Behavior:**

* Associated behavior executed whenever State is exited

**doActivity Behavior:**

* Commences execution when State is entered (after entry Behavior completes)
* Executes concurrently with other Behaviors associated with the State
* Continues until:
  * It completes (generates a completion event), or
  * The State is exited (doActivity Behavior is aborted)

**Important:** Execution of doActivity Behavior is NOT affected by firing of an internal Transition of that State.

#### 14.2.3.4.4 State History

Introduced by David Harel in original statechart formalism. A **convenience concept** where a Region keeps track of the state configuration it was in when last exited.

**Two Types:**

1. **Deep History** (`deepHistory`)
   * Represents the **full state configuration** of most recent visit
   * Effect: as if Transition terminated on innermost State of preserved configuration
   * Includes execution of all entry Behaviors in appropriate order

2. **Shallow History** (`shallowHistory`)
   * Represents return to only **topmost substate** of most recent configuration
   * Entered using default entry rule

**Default History:**

* When Transition terminates on history Pseudostate when State has not been entered before (no prior history)
* Or when State had reached its FinalState
* A default history Transition can force transition to specific substate
* If no default history Transition defined, standard default entry is performed

**Constraints:**

* At most one history Pseudostate per Region of a composite State

#### Deferred Events

A State may specify a set of Event types that may be **deferred** in that State.

**Behavior:**

* Event occurrences of deferred types not dispatched while State remains active
* Remain in event pool until:
  * State configuration reached where Event types no longer deferred, or
  * Deferred Event type used explicitly in Trigger of Transition from deferring State (override option)

An Event may be deferred by composite State or submachine States - remains deferred as long as composite State remains in active configuration.

#### 14.2.3.4.5 Entering a State

Entry semantics depend on State type and manner of entry.

**All cases:**

* Entry Behavior executed (if defined) after any effect Behavior of incoming Transition
* If doActivity Behavior defined, commences after entry Behavior
* Executes concurrently with subsequent Behaviors (like entry Behaviors of substates)

**For Composite States with Single Region:**

1. **Default Entry**
   * Composite State is direct target of Transition
   * After executing entry Behavior and forking doActivity:
     * If initial Pseudostate defined: continues from that Vertex via outgoing Transition
     * If no initial Pseudostate:
       * Option 1: Treat model as ill formed
       * Option 2: Treat composite State as simple State

2. **Explicit Entry**
   * Incoming Transition terminates on directly contained substate
   * That substate becomes active
   * Its entry Behavior executed after containing composite State's entry Behavior
   * Rule applies recursively for deeply nested substates

3. **Shallow History Entry**
   * Incoming Transition terminates on `shallowHistory` Pseudostate
   * Active substate becomes most recently active substate
   * **Exceptions:**
     * Most recently active was FinalState, or
     * First entry into this State
   * In exceptions: use default shallow history Transition if defined, otherwise default entry

4. **Deep History Entry**
   * Same as shallow history but uses `deepHistory` Pseudostate
   * Applied recursively to all levels in active state configuration

5. **Entry Point Entry**
   * Transition enters through `entryPoint` Pseudostate
   * Effect Behavior of outgoing Transition from entry point executed
   * After entry Behavior of composite State

**For Orthogonal States:**

* Each Region is entered (by default or explicitly)
* If Transition terminates on edge: all Regions entered by default
* If Transition explicitly enters one or more Regions (fork): those entered explicitly, others by default

**Important:** StateMachine is deemed "in" a State even before any entry/effect Behaviors start executing.

#### 14.2.3.4.6 Exiting a State

**Exit Process:**

* Final step: execution of exit Behavior (after all other exit-related Behaviors)
* If doActivity Behavior still executing: aborted before exit Behavior

**Composite State Exit:**

* Exit commences with innermost State in active state configuration
* Exit Behaviors executed in sequence from innermost active State
* If exit through `exitPoint` Pseudostate: exit Behavior executed after effect Behavior of Transition terminating on exit point

**Orthogonal State Exit:**

* Each Region is exited
* Then exit Behavior of State executed

**Important:** StateMachine deemed to have "left" a State only after exit Behavior completes.

#### Encapsulated Composite States

**Purpose:** Prevent Transitions from penetrating directly into State to terminate on internal Vertices.

**Use Case:** When internals of a State in abstract Classifier are intended to be specified differently in different subtype refinements.

**Mechanism:** Entry and exit points via `entryPoint` and `exitPoint` Pseudostates.

**Entry Points:**

* Termination points (sources) for incoming Transitions
* Origination points (targets) for Transitions terminating on internal Vertex
* Execution of composite State entry Behavior occurs between effect Behavior of incoming and outgoing Transitions
* If no outgoing Transition inside: incoming Transition performs default State entry

**Exit Points:**

* Inverse of entry points
* Transitions from Vertex within composite State can terminate on exit point
* Should have corresponding external Transition outgoing from same exit point
* If exit Behavior defined: executed after effect Behavior of incoming inside Transition and before effect Behavior of outgoing external Transition

#### 14.2.3.4.7 Submachine States and Submachines

**Purpose:** Means by which a single StateMachine specification can be reused multiple times.

Similar to encapsulated composite States but:

* Encapsulated composite States: contained within StateMachine where defined
* Submachines: distinct Behavior specifications (like programming language macros)
* May be defined in different context than where used

**Binding Mechanism:**

* **Submachine State**: States with `isSubmachineState = true`
* **ConnectionPointReference**: Supports binding between submachine State and referenced StateMachine
* Represents point on submachine State where Transition terminates or originates
* Each ConnectionPointReference matched by corresponding entry/exit point in referenced submachine

**Semantics:**

* Macro-like insertion of submachine StateMachine specification
* Semantically equivalent to composite State
* Regions of submachine StateMachine are Regions of composite State
* Entry, exit, effect Behaviors and internal Transitions defined as contained in submachine State

> **NOTE:** Each submachine State represents distinct instantiation, even when multiple submachine States reference same submachine.

**Entry:**

* Via default (initial) Pseudostate, or
* Via any entry points
* Entry point equivalent to junction Pseudostate (fork for orthogonal composite States)
* Guards on entry point Transitions must evaluate to true for well-formed specification

**Exit:**

* Reaching FinalState
* Triggering group Transition from submachine State
* Via any exit points

***

### 14.2.3.5 ConnectionPointReference

Represents a usage (as part of submachine State) of an entry/exit point defined in StateMachine referenced by the submachine State.

**Purpose:**

* Can be used as sources/targets of Transitions
* Represent entries into or exits out of submachine StateMachine

**Entry Point Connection Point Reference:**

* As target of Transition: implies target is entryPoint Pseudostate in submachine
* Regions of submachine StateMachine entered through corresponding entryPoint Pseudostates

**Exit Point Connection Point Reference:**

* As source of Transition: implies source is exit point Pseudostate in submachine
* When Region reaches corresponding exit point, submachine State exited via this exit point

***

### 14.2.3.6 FinalState

**FinalState** is special kind of State signifying enclosing Region has completed.

A Transition to FinalState represents completion of behaviors of Region containing the FinalState.

***

### 14.2.3.7 Pseudostate and PseudostateKind

**Pseudostate** is abstraction encompassing different types of transient Vertices in StateMachine graph.

Generally used to chain multiple Transitions into more complex compound transitions.

#### Pseudostate Kinds

1. **initial**
   * Starting point for Region
   * Execution commences when Region entered via default activation
   * Source for at most one Transition
   * May have associated effect Behavior, but not trigger or guard
   * At most one initial Vertex per Region

2. **deepHistory**
   * Variable representing most recent active state configuration of owning Region
   * Transition terminating here restores Region to same state configuration
   * With all semantics of entering a State (entry Behaviors in appropriate order)
   * Can only be defined for composite States
   * At most one per Region

3. **shallowHistory**
   * Variable representing most recent active substate (not substates of that substate)
   * Transition terminating here restores Region to that substate
   * Single outgoing Transition may be defined (default shallow history state)
   * Can only be defined for composite States
   * At most one per Region

4. **join**
   * Common target Vertex for two or more Transitions from different orthogonal Regions
   * Transitions to join cannot have guard or trigger
   * Synchronization function: all incoming Transitions must complete before continuing
   * Similar to junction points in Petri nets

5. **fork**
   * Splits incoming Transition into two or more Transitions terminating on Vertices in orthogonal Regions
   * Outgoing Transitions cannot have guard or trigger

6. **junction**
   * Connects multiple Transitions into compound paths between States
   * **Uses:**
     * Merge multiple incoming Transitions into single outgoing (shared continuation)
     * Split incoming Transition into multiple outgoing segments with different guards
   * **Static conditional branch**: guards evaluated before compound transition execution
   * If guards prevent reaching valid state configuration: entire compound transition disabled
   * "else" guard can be associated with at most one outgoing Transition
   * If multiple guards true: one chosen (algorithm undefined)

7. **choice**
   * Similar to junction
   * **Key difference:** Guards evaluated **dynamically** when compound transition reaches this Pseudostate
   * **Dynamic conditional branch**
   * Decision can depend on results of Behaviors executed before reaching choice point
   * If multiple guards true: one chosen (algorithm undefined)
   * If no guards true: model ill formed (use "else" guard to avoid)

8. **entryPoint**
   * Entry point for StateMachine or composite State providing encapsulation
   * At most single Transition from entry point to Vertex within each Region
   * If owning State has entry Behavior: executed before behavior of outgoing Transition
   * If multiple Regions: acts as fork Pseudostate

9. **exitPoint**
   * Exit point of StateMachine or composite State providing encapsulation
   * Transitions terminating on exit point imply exiting composite State/submachine State
   * Exit Behavior of composite State executed
   * If multiple Transitions from orthogonal Regions terminate here: acts like join Pseudostate

10. **terminate**
    * Entering terminate Pseudostate: execution of StateMachine terminated immediately
    * StateMachine does not exit any States
    * No exit Behaviors performed
    * Any executing doActivity Behaviors automatically aborted
    * Equivalent to invoking DestroyObjectAction

***

### 14.2.3.8 Transitions

A **Transition** is single directed arc:

* Originates from single source Vertex
* Terminates on single target Vertex
* Source and target may be same Vertex
* Specifies valid fragment of StateMachine Behavior
* May have associated effect Behavior executed when traversed

> **NOTE:** Duration of Transition traversal is undefined (allows for both "zero" and non-"zero" time interpretations).

Transitions executed as part of more complex **compound transition** that takes StateMachine from one stable state configuration to another.

#### Transition States

A Transition instance is:

* **Reached**: when execution has reached its source Vertex (source State in active state configuration)
* **Traversed**: when being executed (along with effect Behavior)
* **Completed**: after reaching its target Vertex

#### Triggers

* Transition may own set of Triggers
* Each specifies Event whose occurrence, when dispatched, may trigger traversal
* Transition trigger **enabled** if dispatched Event occurrence matches its Event type
* Multiple triggers: logically disjunctive (if any enabled, Transition triggered)

#### 14.2.3.8.1 Transition Kinds Relative to Source

Three possibilities based on `kind` attribute:

1. **external** (`kind = external`)
   * Transition exits its source Vertex
   * If Vertex is State: execution of exit Behavior

2. **local** (`kind = local`)
   * Opposite of external
   * Does NOT exit containing State (no exit Behavior execution)
   * Target Vertex must be different from source Vertex
   * Can only exist within composite State

3. **internal** (`kind = internal`)
   * Special case of local Transition that is self-transition
   * Same source and target States
   * State never exited (not re-entered)
   * No exit or entry Behaviors executed
   * Can only be defined if source Vertex is State

#### 14.2.3.8.2 High-Level (Group) Transitions

**Group Transitions**: Transitions whose source Vertex is composite State.

If external:

* Result in exiting all substates of composite State
* Exit Behaviors executed starting with innermost States in active state configuration

If local:

* Exit Behaviors of source State and entry Behaviors of target State executed
* But not those of containing State

#### 14.2.3.8.3 Completion Transitions and Completion Events

**Completion Transition**: Special kind with implicit trigger.

**Completion Event**: Event that enables this trigger, signifying all Behaviors associated with source State have completed.

**Generation Rules:**

For **simple States**:

* Generated when entry and doActivity Behaviors completed
* If no such Behaviors: generated upon entry

For **composite or submachine States**:

* All internal activities (entry, doActivity) completed, AND
* If composite State: all orthogonal Regions reached FinalState, OR
* If submachine State: submachine StateMachine execution reached FinalState

**Priority:**

* Completion events have dispatching priority
* Dispatched ahead of pending Event occurrences in event pool
* If multiple from orthogonal Regions occur simultaneously: processing order undefined

**Completion of all top level Regions** = completion of StateMachine Behavior → termination

#### Transition Guards

* Transition may have associated guard Constraint
* Transitions with guard evaluating to false are disabled
* Guards evaluated before compound transition enabled (exception: choice Pseudostate)
* For choice Pseudostate: guards evaluated when choice point reached
* No guard: treated as guard always true

> **NOTE:** Completion Transition may also have guard.

**Guard Constraints:**

* May involve tests of orthogonal States
* May test explicitly designated States of reachable object
* Examples: "in State1", "not in State2"
* State names may be fully qualified: "RegionA::State1::Region1::State2::State3"

#### 14.2.3.8.4 Compound Transitions

When Event occurrence triggers enabled Transition or StateMachine execution created:

* Initiates traversal of set of connected and nested Transitions and Vertices
* Until stable state configuration reached

**Compound Transition**: Trace of this traversal, represented by acyclical directed graph.

**Root (source) can be:**

* Transition with one or more Triggers defined
* Completion Transition
* Set of Transitions from different orthogonal Regions converging on common join Pseudostate
* Transition from initial Pseudostate of topmost Region (when StateMachine instance created)

**Branching** occurs:

* When executing Transition performs default entry into State with multiple orthogonal Regions
* When fork Pseudostate encountered

**Behavior Ordering:**

* Partially ordered set of executions determined by order elements encountered
* Example: Transition entering compound State terminating on substate:
  1. Effect Behavior of Transition
  2. Entry Behavior of compound State
  3. Entry Behavior of substate
* Fork Pseudostate: effect Behaviors of branches executed concurrently (conceptually)

**Choice/Join Points:**

* If multiple outgoing Transitions with guards: one whose guard evaluates to true taken
* If multiple guards true: one chosen (algorithm undefined)
* For choice Pseudostate: if no guards true when reached, model ill formed

#### 14.2.3.8.5 Transition Ownership

* Owner not explicitly constrained
* Region containing Transition must be owned directly/indirectly by owning StateMachine
* **Suggested owner**: Innermost Region containing both source and target Vertices

***

### 14.2.3.9 Event Processing for StateMachines

#### 14.2.3.9.1 The Run-to-Completion Paradigm

Event occurrence processing conforms to general semantics in Clause 13.

**Lifecycle:**

1. Upon creation: StateMachine performs initialization
2. Executes initial compound transition prompted by creation
3. Enters wait point (stable state configuration for StateMachine Behaviors)
4. Remains until Event from event pool is dispatched
5. Event evaluated: if matches valid Trigger and at least one enabled Transition:
   * Single StateMachine **step** executed
   * Step = executing compound transition, terminating on stable state configuration (next wait point)
6. Cycle repeats until:
   * StateMachine completes its Behavior, or
   * Asynchronously terminated by external agent

**Event Handling:**

* StateMachines respond to Event types in Clause 13 and completion events
* Completion events have priority (dispatched ahead of pending Event occurrences)
* Event occurrences detected, dispatched, processed one at a time
* Dispatching order undefined (allows varied scheduling algorithms)

**Run-to-Completion Definition:**
In absence of exceptions or asynchronous destruction:

* Pending Event occurrence dispatched only after previous occurrence processing completed
* And stable state configuration reached
* Event occurrence never dispatched while StateMachine busy processing previous one

**Purpose:** Avoid complications from concurrency conflicts when responding to multiple concurrent/overlapping events.

**Transition Enablement:**

* When Event dispatched: may enable one or more Transitions for firing
* If no Transition enabled and Event type not in deferrableTriggers of active state configuration: Event discarded, step completed trivially

**Orthogonal Regions:**

* Multiple Transitions (in different Regions) can be triggered by same Event occurrence
* Execution order undefined
* Each "bottom-level" orthogonal Region can fire at most one Transition per Event occurrence
* When all orthogonal Regions finished: Event fully consumed, step completed

**Mutually Exclusive Transitions:**

* Multiple in given Region may be enabled by same Event occurrence
* Only one selected and executed
* Determined by Transition selection algorithm (below)

**Transition Behaviors:**

* During Transition: number of action Behaviors may execute
* If includes synchronous invocation call on another object with StateMachine:
  * Transition step not completed until invoked object method completes its run-to-completion step

**Implementation:**

* Active Classes: event-loop in own thread reading from event pool
* Passive Classes: monitor implementation

> **IMPLEMENTATION NOTE:** Run-to-completion NOT interpreted as implying StateMachine cannot be interrupted (would cause priority inversion). Thread executing StateMachine step can be suspended for higher-priority threads, then safely resume and complete event processing.

#### 14.2.3.9.2 Enabled Transitions

A Transition is **enabled** if and only if:

1. All source States in active state configuration
2. At least one Trigger has Event matched by dispatched Event occurrence type
   * Signal Events: any occurrence of same or compatible type matches
   * AnyReceiveEvent: Signal or CallEvent satisfies (provided no other Signal/CallEvent Trigger for same Transition or any other Transition with same source Vertex)
3. At least one full path exists from source to either:
   * Target state configuration, or
   * Dynamic choice Pseudostate
   * With all guard conditions true (no guards treated as always true)

Being enabled = necessary but not sufficient condition for firing (more than one Transition may be enabled by same Event).

#### 14.2.3.9.3 Conflicting Transitions

Multiple Transitions may be enabled within StateMachine, potentially in conflict.

**Example:** Two Transitions from same State, triggered by same event, with different guards. If both guards true, at most one fires in given run-to-completion step.

**Definition:** Two Transitions conflict if they both exit the same State (or intersection of States they exit is non-empty).

**Constraint:** Only Transitions in mutually orthogonal Regions may fire simultaneously. Guarantees new active state configuration is well formed.

**Internal Transition:** Conflicts only with Transitions causing exit from that State.

#### 14.2.3.9.4 Firing Priorities

When conflicting Transitions exist, selection based in part on implicit priority.

Priorities resolve some but not all conflicts (define partial ordering only).

**Priority Rule:** Based on relative position in state hierarchy.

* Transition from substate has higher priority than conflicting Transition from any containing States

**Definition:** Priority of Transition defined based on source State.

* Priority of Transitions chained in compound transition: based on Transition with most deeply nested source State

**General Rules:**

* If t1 has source s1, t2 has source s2:
  * If s1 is direct or indirectly nested substate of s2: t1 has higher priority than t2
  * If s1 and s2 not in same state configuration: no priority difference

#### 14.2.3.9.5 Transition Selection Algorithm

**Set of Transitions that will fire** must satisfy:

1. All Transitions in set are enabled
2. No conflicting Transitions within set
3. No Transition outside set has higher priority than Transition in set

**Implementation:** Greedy selection algorithm with straightforward traversal of active state configuration.

**Traversal Process:**

* States in active state configuration traversed from innermost nested simple States outward
* For each State at given level: all originating Transitions evaluated for enablement
* Guarantees priority principle not violated

**Resolving Conflicts:** Terminate search in each orthogonal State once Transition inside any component is fired.

#### 14.2.3.9.6 Transition Execution Sequence

Every Transition (except internal and local) causes:

* Exiting of source State
* Entering of target State

**Main Source and Main Target:**

* **Main source**: Direct substate of Region containing source States
* **Main target**: Substate of Region containing target States

> **NOTE:** Transition from one Region to another in same immediate enclosing composite State not allowed.

**Execution Steps (in order):**

1. **Exit Phase**
   * Starting with main source State
   * States containing main source State exited according to State exit rules
   * Series of exits continues until reaching **least common ancestor** (Region containing directly/indirectly both main source and main target states)

2. **Transition Effect**
   * Effect Behavior of Transition connecting source sub-configuration to target sub-configuration executed

3. **Entry Phase**
   * Configuration of States containing main target State entered
   * Starting with outermost State in least common ancestor Region
   * Follows State entry (or composite State entry) rules

**Example from Figure 14.2:**
When event "sig" dispatched while in State "S11" (main source):

```
Execution sequence: xS11; t1; xS1; t2; eT1; eT11; t3; eT111
```

(Where x = exit, e = entry, t = transition effect)

***

## 14.2.4 Notation

### 14.2.4.1 StateMachine Diagrams

StateMachine diagrams specify StateMachines through graphic elements:

* **States and Vertices**: Rendered by appropriate State and Pseudostate symbols
* **Transitions**: Generally rendered by directed arcs connecting them, or control symbols

### 14.2.4.2 StateMachine

When depicting StateMachine redefinition in class diagram:

* Use default rectangle notation for Classifier
* Keyword `«statemachine»` inside name compartment (above or before name)
* Association between StateMachine and context Classifier/BehavioralFeatures: no special graphical representation

### 14.2.4.3 Region

**Composite State or StateMachine with Regions:**

* Graph Region tiled using dashed lines to divide into Regions
* Each Region may have optional name
* Contains nested disjoint States and Transitions between them
* Text compartments separated from orthogonal Regions by solid line

**Single Region:**

* Shown by nested state diagram within graph Region

### 14.2.4.4 State

**Basic Notation:**

* Rectangle with rounded corners
* State name shown within

**Optional Name Tab:**

* Rectangle resting on outside of top side of State
* Contains State name
* Normally used for composite State with orthogonal Regions

**Compartments:**
States may be subdivided into compartments separated by horizontal lines:

1. **Name Compartment**
   * Holds optional name of State (string)
   * States without names are anonymous and distinct
   * Avoid showing same named State twice in diagram
   * Not used if name tab used (and vice versa)
   * For submachine State: name of referenced StateMachine shown as string following ':' after State name

2. **Internal Behaviors Compartment**
   * List of internal Behaviors
   * Format: `<behavior-type-label> ['/' <behavior-expression>]`
   * Labels:
     * `entry` — Performed upon entry to State
     * `exit` — Performed upon exit from State
     * `do` — Ongoing Behavior performed while in State (until computation completes)

3. **Internal Transition Compartment**
   * List of internal Transitions
   * Syntax: `{<trigger>}* ['[' <guard>']'] [/<behavior-expression>]`
   * `<trigger>`: Trigger notation
   * `<guard>`: Boolean expression
   * `<behavior-expression>`: Effect Behavior specification

**Alternative Representation:**
Behaviors can be expressed using appropriate graphical representation in separate diagram (e.g., activity diagram).

#### 14.2.4.4.1 Composite State

**Decomposition Compartment:**

* Shows composition structure (Regions, States, Transitions)
* In addition to name and internal Transition compartments
* Text compartments may be shrunk horizontally within graphic Region

**Hidden Decomposition:**
When convenient to hide decomposition (e.g., too many nested States):

* Composite State represented by simple State graphic
* Special "composite" icon in lower right-hand corner
* Icon: two horizontally placed and connected States
* Optional visual cue that State has decomposition not shown
* Contents shown in separate diagram

> **NOTE:** "Hiding" is purely graphical convenience, no semantic significance for access restrictions.

**Entry and Exit Points:**

* Composite State may have one or more entry and exit points
* On outside border or in close proximity (inside or outside)

***

## Summary

This specification defines the comprehensive semantics and notation for UML StateMachines, including:

* **Hierarchical state structures** with nested regions and substates
* **Event-driven execution** following run-to-completion semantics
* **Complex state transitions** including compound, completion, and group transitions
* **History mechanisms** for returning to previous state configurations
* **Orthogonal regions** for concurrent state machine execution
* **Pseudostates** for control flow (fork, join, junction, choice, entry/exit points)
* **Submachines** for reusable state machine components
* **Comprehensive notation** for visualizing state machine structures

The specification provides the foundation for implementing robust, hierarchical state machines with well-defined execution semantics.
