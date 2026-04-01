# Claude for Software Development
#### CGP - 1 April 2026

As of this date I have begun to use Claude Code to help software development. These instructions may change as I explore more.

## Claude Code Installation

This is beynd the scope of this document. However, I recorded a conversation I had with Claude 
[here](https://claude.ai/chat/83b6dabd-6be4-452a-8ed7-50103d4f8d20) and it might be useful. 
This was some time ago and things might have changed.

It does say this:
```
Since you're on Windows, you'll need to use Windows Subsystem for Linux (WSL) as Claude Code doesn't run directly on Windows yet.

* Install WSL if you don't have it already
* Install Node.js 18+ in your WSL environment
```

Windows Subsystem for Linux (WSL) is a Microsoft feature that allows you to run a native GNU/Linux environment directly on Windows 10/11, without needing a separate virtual machine or dual-boot setup.

## Setting Up Instructions

Interacting with Claude usually involves creating insrcutions in a `CLAUDE.md` markdown file.

At Claude's suggestion I have this structure:

1.  A generic `CLAUDE.md` file at the root of the development folder - in here: `Seeed_Grove_Vision_AI_Module_V2\CLAUDE.md`. This provides an overview of the SDK and importanty includes these lines - the `@` symbol indicating an "import" of further instructions.:

```
## Task-specific instructions

Current task instructions are imported from the ww500_md project directory.
When there is no active task, this import line can be removed or commented out.

@EPII_CM55M_APP_S/app/ww_projects/ww500_md/claude/CLAUDE.md
```

2.  A task-specific `CLAUDE.md` file which I have placed in a sub-directory of the project. I can edit this file for each task I want to give to Claude.  So far I have asked Claude to do this:

* Analyse my instructions and the project files and produce a report, with recommendations, as a markdown file for me to review.


## Launching and Running Claude

To start:

1.  I open a command prompt at the root of the SDK
2.  I run `wsl` to start WSL.
3.  I type `claude` to start Claude Code.
4.  When I am sure about the instructions mentioned above, I tell Claude to carry out the instructions.
5.  Since my task-specific `CLAUDE.md` file asks for an analysis, I expect a markdown file from Claude to review.
6. If and when I am happy with this analysis I can ask Claude to implement the recommendations. 
7. At present I have to run the compiler, but it might be possible for Claude to do this. I build and test the firmware, iterate if necessary.  
8. When the task is completed I can retire the instructions, by renaming the file. For example, I renamed `CLAUDE.md` to `CLAUDE_deployment_id.md` when the task was complete. 

