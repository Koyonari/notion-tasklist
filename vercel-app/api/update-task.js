import { Client } from "@notionhq/client";

const notion = new Client({ auth: process.env.NOTION_TOKEN });
const STATUS_NAMES = ["To Do", "Doing", "Done"];

export default async function handler(req, res) {
  if (req.method !== "PATCH") return res.status(405).end();

  const { id, status } = req.body;
  if (!id || status === undefined || status < 0 || status > 2) {
    return res.status(400).json({ error: "Invalid id or status" });
  }

  try {
    await notion.pages.update({
      page_id: id,
      properties: {
        Status: { status: { name: STATUS_NAMES[status] } },
      },
    });
    res.status(200).json({ ok: true });
  } catch (err) {
    console.error(err);
    res.status(500).json({ error: "Failed to update task" });
  }
}
