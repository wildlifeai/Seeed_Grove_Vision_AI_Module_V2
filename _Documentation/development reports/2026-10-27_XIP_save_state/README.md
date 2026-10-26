# Save State in XIP Memory

#### File: `README.md`
#### Path: `_Documentation\development reports\2026-10-27_XIP_save_state\README.md`
#### Author: Charles Palmer
#### Date: 27 October 2026


## Background

The github.com repo contains a large number of issues and TODO items. 

Many of these will have been dealt with already, but some still need work.

As of the time of writing, the mis-named `1261023_mdEnhancements` branch is the 'latest'
branch on my laptop, as PRs have not yet been finalised on github.com. I have created the 
`261027_XIP_save_state` branch from `1261023_mdEnhancements` 

The TODO list is [here](https://github.com/orgs/wildlifeai/projects/3/views/3?filterQuery=repo%3Awildlifeai%2FSeeed_Grove_Vision_AI_Module_V2) 

--- 

## Save State in XIP Memory

After Claude created `CLAUDE_todo_summary_and_plan.md` it is apparent that many of the TODO items relate 
to saving state in the XIP memory rather than (or as well as) the SD card.

So this development thread has been re-purposed, form `Deal with outstanding TODO items`
to `Save State in XIP Memory`
 
Github already contains ideas about doing this - these should be referenced.
 
See new instructions for Claude below.

## Clock adjustments

We tuned the HX6538 clock on a single board, to reduce drift.

This could usefully be done on every board, but I suspect that the drift will differ between boards,
so the adjustment becomes unique to each board. The adjustment value could be placed into XIP memory.

---

## Task for Claude

1.	Read this document and the items in the TODO list on github.com
2.	Create a markdown document `CLAUDE_todo_summary_and_plan.md` that summarises each of the TODO items. 
Place it in the same folder as this document. 
In particular the document should say whether they have been dealt with already. 
If they have not been dealt with, provide a simple jargon-free description of the problem.
3.	Then, propose a sequece for dealing with the TODO iitems raised above in a series of 
steps that we can carry out one at a time. 

** After creation of `CLAUDE_todo_summary_and_plan.md`:

4. Leave `CLAUDE_todo_summary_and_plan.md` as a list of work to do.
5. Create a new proposal document to use XIP memory to save some operational parameters and also the clock adjustment settings.
Include a brief summary of the points already documented, but write them afresh, and succinctly.
6. Ask questions as required.
7. Don't write any code until after we have both reviewed that proposal document.
