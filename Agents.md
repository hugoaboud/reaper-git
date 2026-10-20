# Reaper Git

`Reaper Git` is an embedded git client with SSH support for the Reaper DAW.

- **Read the Docs**

The folder `docs` contains markdown files describing the overall structure of the project.
Read the files closely related to what's being changed before analysing or changing any code.
Follow the docs strictly.

- **Write the Docs**

Make sure to keep the docs files updated.
- Use concise english language without abbreviating too much
- Don't use technical terms where an informal one is accurate enough
- Be descriptive without over-narrating

Each file should be structured as a knowledge tree, containing a ToC on the top linking to the Markdown sections, right after the title.
The tree nodes should have consistent categorization, so that children nodes are all of the same "category", and sibling nodes can expand context.

Make sure to use a layout that consistent through the different markdown files to display information, so that the same kind of information can be found on the similar markdown contexts across files.
Always use the `ask` tool when documenting to check for the validity of specs.

- **Plan before Write**

Always investigate and setup a plan to cover the following:
- I have enough evidence of the issue
- I have at least one understanding of what could be causing the issue
- I have enough evidence of the solution, coming from - in order of preference:
    - Project files
    - References/documentation
    - Third party libraries inside the project
    - Web search (StackOverflow or other technical communities)
    - LLM dataset knowledge
- I have evidence that the technical fix solves the use case
- I have a clear path on how to reach the end goal in the least amount of turns

If the plan doesn't cover this and development gets stuck, use the `ask` tool.