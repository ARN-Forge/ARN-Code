\# ARN — Agents \& Multi-Model Roadmap



\## Purpose



This document describes the long-term architecture planned for ARN.



IMPORTANT:

This is a ROADMAP, not a request to implement everything immediately.



When working on ARN:

\- Preserve compatibility with this roadmap where reasonable.

\- Implement features incrementally.

\- Do not prematurely build future systems unless the current task requires them.

\- Prefer clean interfaces that will allow later stages to be added without major rewrites.

\- Keep ARN lightweight and native C++23.

\- Do not introduce Node.js or Python runtime dependencies.



\---



\# 1. Long-Term Goal



ARN should evolve from a single-model CLI coding agent into a modular

multi-provider, multi-model agent system.



Eventually ARN should be able to:



1\. Support multiple AI providers.

2\. Support multiple models from each provider.

3\. Define specialized agent profiles.

4\. Define reusable skills.

5\. Automatically select agents for tasks.

6\. Automatically select appropriate models for agents/tasks.

7\. Use cheaper models for simple work and stronger models for difficult work.

8\. Escalate tasks to stronger models when weaker models fail.

9\. Allow users to control cost, quality and routing behavior.

10\. Eventually support real subagents with isolated contexts.



These features MUST be implemented gradually.



\---



\# 2. Phase 1 — Clean Provider / Model Architecture



This is the foundation.



ARN should have a provider abstraction so that the rest of the application

does not depend directly on Gemini, DeepSeek or another provider.



Conceptually:



&#x20;   ARN

&#x20;    |

&#x20;    +-- Model/Provider abstraction

&#x20;         |

&#x20;         +-- Gemini

&#x20;         +-- DeepSeek

&#x20;         +-- Future providers



Agent logic should never need to understand provider-specific HTTP APIs.



Provider implementations should handle things such as:



\- authentication

\- API requests

\- streaming

\- model discovery

\- provider-specific errors

\- rate limits

\- model capabilities



The rest of ARN should use a common interface.



Do NOT over-engineer this abstraction before it is needed.



\---



\# 3. Phase 2 — Agent Profiles



Add specialized agent profiles.



Initial agents could include:



&#x20;   Explorer

&#x20;   Planner

&#x20;   Coder

&#x20;   Debugger

&#x20;   Reviewer



An agent is NOT necessarily another AI model.



Initially, agents may simply be different profiles using the same underlying

model.



Each profile may define:



\- system instructions

\- allowed tools

\- permissions

\- skills

\- preferred model/model class

\- behavioral rules



Example:



&#x20;   Explorer

&#x20;     tools: read/search/list

&#x20;     write access: false



&#x20;   Coder

&#x20;     tools: read/search/edit/create

&#x20;     write access: true



&#x20;   Debugger

&#x20;     tools: read/search/shell/edit

&#x20;     write access: true



&#x20;   Reviewer

&#x20;     tools: read/search

&#x20;     write access: false



User commands may eventually include:



&#x20;   /agent explorer

&#x20;   /agent coder

&#x20;   /agent debugger

&#x20;   /agent reviewer

&#x20;   /agent auto



Manual selection should remain available even after automatic routing exists.



\---



\# 4. Phase 3 — Skills



Skills are reusable task-specific instructions.



Agents and skills are different concepts.



Agent:

&#x20;   Defines WHO is performing the task and what permissions/tools it has.



Skill:

&#x20;   Defines HOW a particular type of task should be approached.



Possible examples:



&#x20;   skills/

&#x20;     cpp-debug/

&#x20;       SKILL.md



&#x20;     cmake/

&#x20;       SKILL.md



&#x20;     linux-portability/

&#x20;       SKILL.md



&#x20;     security-review/

&#x20;       SKILL.md



&#x20;     git/

&#x20;       SKILL.md



An agent may load one or more relevant skills.



Example:



&#x20;   Debugger

&#x20;     + cpp-debug

&#x20;     + cmake

&#x20;     + linux-portability



Do not load every skill into every context.

Only relevant skills should be included.



\---



\# 5. Phase 4 — Automatic Agent Routing



ARN should eventually have an orchestrator.



The user should be able to give ARN a normal task without manually selecting

every agent.



Example:



&#x20;   User:

&#x20;   "Find why ARN crashes after Ctrl+C on Linux and fix it."



Possible workflow:



&#x20;   Orchestrator

&#x20;       |

&#x20;       +--> Explorer

&#x20;       |

&#x20;       +--> Debugger

&#x20;       |

&#x20;       +--> Coder

&#x20;       |

&#x20;       +--> Reviewer



The orchestrator should decide which role is appropriate for each stage.



Manual agent selection must still be possible.



\---



\# 6. Phase 5 — Multi-Model Agent Routing



Agents must NOT be permanently tied to one model.



Example:



&#x20;   Explorer -> fast/cheap model

&#x20;   Planner  -> reasoning model

&#x20;   Coder    -> coding model

&#x20;   Debugger -> strong reasoning/coding model

&#x20;   Reviewer -> fast/cheap model



Users may have models from multiple providers.



Example:



&#x20;   Gemini

&#x20;   DeepSeek

&#x20;   Future providers



The architecture should allow each agent invocation to use a different model.



\---



\# 7. Phase 6 — Smart Model Router



Eventually ARN should automatically choose a model based on the task.



The router may consider:



\- task type

\- model capabilities

\- coding ability

\- reasoning ability

\- context window

\- speed

\- cost

\- previous failures

\- provider availability

\- rate limits

\- user preferences



Example:



&#x20;   Repository exploration

&#x20;       -> fast inexpensive model



&#x20;   Difficult debugging

&#x20;       -> stronger reasoning/coding model



&#x20;   Simple code review

&#x20;       -> inexpensive model



The orchestrator decides WHAT work is needed.



The model router decides WHICH MODEL should perform that work.



Keep these responsibilities separate.



\---



\# 8. Routing Modes



Possible future modes:



&#x20;   /model-mode quality

&#x20;   /model-mode balanced

&#x20;   /model-mode economy



Quality:

&#x20;   Prefer the strongest appropriate models.



Balanced:

&#x20;   Use inexpensive models for routine work and stronger models when needed.



Economy:

&#x20;   Prefer free/inexpensive models and escalate only when necessary.



Users must always remain in control of spending.



\---



\# 9. Model Escalation



ARN may eventually escalate a task when a model fails.



Example:



&#x20;   Cheap model

&#x20;       |

&#x20;       | failed

&#x20;       v

&#x20;   Better coding model

&#x20;       |

&#x20;       | failed

&#x20;       v

&#x20;   Strong reasoning model



Failure signals may include:



\- build failure

\- test failure

\- repeated unsuccessful attempts

\- inability to complete the requested task

\- explicit low confidence



Avoid infinite retry/escalation loops.



\---



\# 10. Cost Protection



Automatic routing must NEVER mean unlimited automatic spending.



Future configuration could include:



&#x20;   max\_cost\_per\_task

&#x20;   allowed\_providers

&#x20;   allowed\_models

&#x20;   blocked\_models

&#x20;   require\_confirmation\_above\_cost



Users should be able to prevent expensive models from being selected

automatically.



\---



\# 11. Model Capability Registry



ARN may maintain metadata about available models.



Conceptually:



&#x20;   ModelCapabilities

&#x20;   {

&#x20;       coding

&#x20;       reasoning

&#x20;       exploration

&#x20;       context\_size

&#x20;       speed\_class

&#x20;       cost\_class

&#x20;       tool\_support

&#x20;   }



Do not hard-code application logic around specific marketing model names.



Models change frequently.



Prefer capability-based routing.



\---



\# 12. Phase 7 — Real Subagents



This is a LATER feature.



Initially, agent profiles may share the main conversation/context.



Eventually agents may run with isolated context windows.



Example:



&#x20;   Main Agent

&#x20;      |

&#x20;      +-- Explorer context

&#x20;      |

&#x20;      +-- Debugger context

&#x20;      |

&#x20;      +-- Reviewer context



A subagent should return a concise result to the parent agent instead of

polluting the main context with everything it read.



Possible benefits:



\- reduced context pollution

\- parallelizable work

\- specialization

\- cheaper model usage

\- better handling of large repositories



Do NOT implement this prematurely.



\---



\# 13. Security / Permission Boundary



Permissions must be enforced by ARN itself.



Never rely only on model instructions.



Example:



If Explorer is read-only, the model must physically not receive access to

write tools.



Agent switching must NOT allow privilege escalation.



Existing ARN protections should remain global, including:



\- working-directory restrictions

\- sensitive path protection

\- confirmation before destructive/file-changing operations

\- API key protection



The orchestrator cannot bypass these restrictions.



\---



\# 14. Possible Future Configuration



Illustrative only:



&#x20;   agents:

&#x20;     explorer:

&#x20;       model: auto

&#x20;       permissions: read-only



&#x20;     coder:

&#x20;       model: auto

&#x20;       permissions: read-write



&#x20;     debugger:

&#x20;       model: auto

&#x20;       permissions: read-write-shell



&#x20;     reviewer:

&#x20;       model: auto

&#x20;       permissions: read-only



&#x20;   routing:

&#x20;     mode: balanced

&#x20;     max\_cost\_per\_task: 0.20



This format is NOT final and should not be implemented merely because it

appears in this roadmap.



\---



\# 15. Development Rule



When implementing any part of this roadmap:



1\. Inspect the current ARN architecture first.

2\. Identify the smallest useful next step.

3\. Make that step work correctly.

4\. Build and test it.

5\. Preserve existing behavior unless change is intentional.

6\. Avoid implementing unrelated future phases.

7\. Keep interfaces extensible where doing so is reasonably simple.

8\. Do not perform speculative large-scale rewrites solely for future features.



A typical implementation order should therefore be:



&#x20;   Provider abstraction

&#x20;       ↓

&#x20;   Multi-provider improvements

&#x20;       ↓

&#x20;   Agent profiles

&#x20;       ↓

&#x20;   Skills

&#x20;       ↓

&#x20;   Agent auto-routing

&#x20;       ↓

&#x20;   Multi-model routing

&#x20;       ↓

&#x20;   Smart routing + escalation

&#x20;       ↓

&#x20;   Real isolated subagents



The roadmap describes the destination.



The CURRENT TASK determines what should actually be implemented.

